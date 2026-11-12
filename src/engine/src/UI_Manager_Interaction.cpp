#include "PCH.h"
#include "UI_Manager.h"
#include "EngineContext.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include <algorithm>

static bool IsContinuationByte(char byte)
{
    constexpr char kContinuationMask = '\xC0';
    constexpr char kContinuationBits = '\x80';
    return (byte & kContinuationMask) == kContinuationBits;
}

static size_t CharacterCount(const std::string& value)
{
    size_t count = 0;
    for (char byte : value)
        if (!IsContinuationByte(byte)) ++count;
    return count;
}

static size_t ByteOffset(const std::string& value, size_t character)
{
    for (size_t offset = 0; offset < value.size(); ++offset) {
        if (IsContinuationByte(value[offset])) continue;
        if (character == 0) return offset;
        --character;
    }
    return value.size();
}

static void EraseCharacter(std::string& value, size_t character)
{
    const size_t begin = ByteOffset(value, character);
    value.erase(begin, ByteOffset(value, character + 1) - begin);
}

static float CaretLength(UISize mode, float length, float line_height)
{
    return mode == UISize::Percent ? line_height * length / 100.0f : length;
}

void UI_Manager::OnClick(const std::string& node_id, UIClickFunc fn)
{
    click_instructions[node_id] = std::move(fn);
}

static float& AxisScroll(UIScrollView& view, UIAxis axis)
{
    return axis == UIAxis::Y ? view.scroll_y : view.scroll_x;
}

static float Along(glm::vec2 point, UIAxis axis)
{
    return axis == UIAxis::Y ? point.y : point.x;
}

static float PageDirection(const UIThumbPlace& place, float point)
{
    if (point < place.thumb_begin) return -1.0f;
    if (point > place.thumb_begin + place.thumb_length) return 1.0f;
    return 0.0f;
}

void UI_Manager::PressAt(float window_x, float window_y, bool shift)
{
    if (!mouse_enabled) return;
    const UIHit hit = HitAt(window_x, window_y);
    const bool on_scrollbar = hit.node && std::holds_alternative<UIScrollbarData>(nodes[hit.node.value].kind);
    press_on_ui  = bool(hit.node);
    pressed_node = on_scrollbar ? UINodeIndex{} : hit.clickable;
    SetFocus(pressed_node);
    if (focused_node) caret = CaretAt(window_x, window_y);
    if (on_scrollbar) GrabScrollbar(hit.node, window_x, window_y, shift);
}

void UI_Manager::ReleaseAt(float window_x, float window_y)
{
    const UINodeIndex released_node = ClickableNodeAt(window_x, window_y);
    if (released_node && released_node == pressed_node)
        pending_clicks.push_back({ nodes[released_node.value].id, nodes[released_node.value].instance });
    CancelPress();
}

void UI_Manager::CancelPress()
{
    press_on_ui    = false;
    pressed_node   = {};
    scrollbar_drag = {};
}

void UI_Manager::SetMouseEnabled(bool enabled)
{
    mouse_enabled = enabled;
    if (!enabled) CancelPress();
}

bool UI_Manager::WantCaptureMouse(float window_x, float window_y) const
{
    if (!mouse_enabled) return false;
    return press_on_ui || focused_node || IsOverUI(window_x, window_y);
}

void UI_Manager::SetFocus(UINodeIndex node)
{
    focused_node = InputVarOf(node, focused_var, focused_key) ? node : UINodeIndex{};
    if (!focused_node) return;
    focused_prefix    = InputPrefixLength(node);
    caret_blink_start = SDL_GetTicks();
}

size_t UI_Manager::InputPrefixLength(UINodeIndex node) const
{
    const std::vector<UITemplatePart>* text = elements[nodes[node.value].element.value].Text();
    if (!text) return 0;
    size_t length = 0;
    for (const UITemplatePart& part : *text) {
        if (std::holds_alternative<UIVarReference>(part)) break;
        length += CharacterCount(PartValue(part, nodes[node.value].instance));
    }
    return length;
}

size_t UI_Manager::CaretAt(float window_x, float window_y) const
{
    const UINode& entry = nodes[focused_node.value];
    const UIInputData* input = std::get_if<UIInputData>(&entry.kind);
    if (!input) return 0;
    const std::vector<UIGlyphPlace>& places = input->text.glyph_places;
    const glm::vec2 point = NodePoint(focused_node, window_x, window_y);
    const float x = point.x + 0.5f * entry.layout_width;
    const float y = 0.5f * entry.layout_height - point.y;
    const int line = std::clamp(safe_sint32(y / input->text.shape->line_height), 0, places.back().line);

    size_t record = places.size() - 1;
    for (size_t i = 0; i + 1 < places.size(); ++i) {
        if (places[i].line < line) continue;
        if (places[i + 1].line != line || x < 0.5f * (places[i].x + places[i + 1].x)) { record = i; break; }
    }
    const size_t length = CharacterCount(GetVar(focused_var, focused_key));
    return std::clamp(record, focused_prefix, focused_prefix + length) - focused_prefix;
}

void UI_Manager::ClearFocus()
{
    focused_node = {};
}

bool UI_Manager::WantCaptureKeyboard() const
{
    return bool(focused_node);
}

void UI_Manager::HandleInput(const std::vector<UIInputEvent>& events)
{
    UIWheelInput wheel;
    for (const UIInputEvent& event : events)
        if (const UIWheelInput* turn = std::get_if<UIWheelInput>(&event))
            wheel = UIWheelInput{ wheel.x + turn->x, wheel.y + turn->y, turn->window_x, turn->window_y };
    ScrollByWheel(wheel);
    TypeIntoFocused(events);
}

void UI_Manager::TypeIntoFocused(const std::vector<UIInputEvent>& events)
{
    if (focused_node && !IsNodeShown(focused_node)) ClearFocus();
    if (!focused_node) return;

    for (const UIInputEvent& event : events) {
        const UITextInput* typed = std::get_if<UITextInput>(&event);
        const UIKeyInput*  key   = std::get_if<UIKeyInput>(&event);
        if (!typed && !key) continue;
        caret_blink_start = SDL_GetTicks();
        std::string value = GetVar(focused_var, focused_key);
        caret = std::min(caret, CharacterCount(value));
        if (typed) {
            value.insert(ByteOffset(value, caret), typed->text);
            caret += CharacterCount(typed->text);
        } else switch (key->scancode) {
            case SDL_SCANCODE_RETURN:    ClearFocus(); return;
            case SDL_SCANCODE_BACKSPACE: if (caret > 0) EraseCharacter(value, --caret); break;
            case SDL_SCANCODE_DELETE:    EraseCharacter(value, caret); break;
            case SDL_SCANCODE_LEFT:      if (caret > 0) --caret; break;
            case SDL_SCANCODE_RIGHT:     if (caret < CharacterCount(value)) ++caret; break;
            case SDL_SCANCODE_HOME:      caret = 0; break;
            case SDL_SCANCODE_END:       caret = CharacterCount(value); break;
            default: break;
        }
        SetVar(focused_var, focused_key, value);
    }
}

UINodeIndex UI_Manager::WheelTargetAt(const UIWheelInput& wheel) const
{
    UINodeIndex target_x, target_y;
    UINodeIndex start = HitAt(wheel.window_x, wheel.window_y).node;
    if (const UIScrollbarData* scrollbar = start ? std::get_if<UIScrollbarData>(&nodes[start.value].kind) : nullptr)
        if (IsNodeShown(scrollbar->target)) start = scrollbar->target;
    for (UINodeIndex node = start; node; node = nodes[node.value].parent) {
        const UIScrollView* view = ScrollViewOf(node);
        if (!view) continue;
        if (!target_x && wheel.x != 0.0f && view->range_x > 0.0f) target_x = node;
        if (!target_y && wheel.y != 0.0f && view->range_y > 0.0f) target_y = node;
    }
    if (!target_x || !target_y) return target_x ? target_x : target_y;
    return wheel_average.x > wheel_average.y ? target_x : target_y;
}

void UI_Manager::ScrollByWheel(const UIWheelInput& wheel)
{
    constexpr float kAverageSamples     = 30.0f;
    constexpr float kTargetHoldMs       = 700.0f;
    constexpr float kTargetHoldDistance = 6.0f;
    constexpr float kStepX              = 26.0f;
    constexpr float kStepY              = 64.0f;
    constexpr float kStepShareLimit     = 0.67f;

    wheel_average += (glm::abs(glm::vec2(wheel.x, wheel.y)) - wheel_average) / kAverageSamples;
    if (!mouse_enabled || (wheel.x == 0.0f && wheel.y == 0.0f)) return;

    const uint64_t  now   = SDL_GetTicks();
    const glm::vec2 point(wheel.window_x, wheel.window_y);
    if (wheel_target && (now >= wheel_target_release || glm::distance(point, wheel_target_point) > kTargetHoldDistance ||
                         !IsNodeShown(wheel_target) || !ScrollViewOf(wheel_target)))
        wheel_target = {};
    const UINodeIndex target = wheel_target ? wheel_target : WheelTargetAt(wheel);
    UIScrollView* view = target ? ScrollViewOf(target) : nullptr;
    if (!view) return;

    bool scroll_x = wheel.x != 0.0f && view->range_x > 0.0f;
    bool scroll_y = wheel.y != 0.0f && view->range_y > 0.0f;
    if (scroll_x && scroll_y) {
        if (wheel_average.x > wheel_average.y) scroll_y = false;
        else                                   scroll_x = false;
    }
    if (!scroll_x && !scroll_y) return;

    const UIContentBox content = nodes[target.value].ContentBox();
    if (scroll_x) {
        const float step = std::trunc(std::min(kStepX, kStepShareLimit * (content.right - content.left)));
        view->scroll_x = std::clamp(view->scroll_x + wheel.x * step, 0.0f, view->range_x);
    }
    if (scroll_y) {
        const float step = std::trunc(std::min(kStepY, kStepShareLimit * (content.bottom - content.top)));
        view->scroll_y = std::clamp(view->scroll_y - wheel.y * step, 0.0f, view->range_y);
    }
    emit_needed = true;

    if (target != wheel_target) {
        wheel_target         = target;
        wheel_target_point   = point;
        wheel_target_release = now;
    }
    const float    turn      = std::min(std::abs(scroll_x ? wheel.x : wheel.y), 1.0f);
    const uint64_t remaining = wheel_target_release > now ? wheel_target_release - now : 0;
    wheel_target_release = now + std::min<uint64_t>(remaining + safe_f_u32(std::round(turn * kTargetHoldMs)),
                                                    safe_f_u32(kTargetHoldMs));
}

UIScrollView* UI_Manager::DraggableView(UINodeIndex bar, UIThumbPlace& place)
{
    if (!IsNodeShown(bar)) return nullptr;
    const UINode& entry = nodes[bar.value];
    const UIScrollbarData* scrollbar = std::get_if<UIScrollbarData>(&entry.kind);
    assert(scrollbar);
    if (glm::determinant(glm::mat2(entry.local)) == 0.0f || !IsNodeShown(scrollbar->target)) return nullptr;
    UIScrollView* view = ScrollViewOf(scrollbar->target);
    place = ThumbPlaceOf(entry, *scrollbar);
    return view && place.range > 0.0f && place.thumb_length < place.track_length ? view : nullptr;
}

glm::vec2 UI_Manager::LayoutPoint(UINodeIndex node, float window_x, float window_y) const
{
    const UINode& entry = nodes[node.value];
    const glm::vec2 point = NodePoint(node, window_x, window_y);
    return glm::vec2(entry.layout_left + 0.5f * entry.layout_width  + point.x,
                     entry.layout_top  + 0.5f * entry.layout_height - point.y);
}

UINodeIndex UI_Manager::ThumbAt(float window_x, float window_y) const
{
    const UINodeIndex bar = HitAt(window_x, window_y).node;
    const UIScrollbarData* scrollbar = bar ? std::get_if<UIScrollbarData>(&nodes[bar.value].kind) : nullptr;
    if (!scrollbar) return {};
    const UIThumbPlace place = ThumbPlaceOf(nodes[bar.value], *scrollbar);
    if (place.thumb_length >= place.track_length) return {};

    UIContentBox thumb = nodes[bar.value].ContentBox();
    const float thumb_end = place.thumb_begin + place.thumb_length;
    if (scrollbar->axis == UIAxis::Y) { thumb.top  = place.thumb_begin; thumb.bottom = thumb_end; }
    else                              { thumb.left = place.thumb_begin; thumb.right  = thumb_end; }
    const glm::vec2 point = LayoutPoint(bar, window_x, window_y);
    return thumb.Contains(point.x, point.y) ? bar : UINodeIndex{};
}

void UI_Manager::GrabScrollbar(UINodeIndex bar, float window_x, float window_y, bool shift)
{
    constexpr uint64_t kPageRepeatDelayMs = 275;

    UIThumbPlace place;
    UIScrollView* view = DraggableView(bar, place);
    if (!view) return;
    const float point     = Along(LayoutPoint(bar, window_x, window_y), place.axis);
    const float direction = PageDirection(place, point);
    if (shift || direction == 0.0f) {
        const float offset = shift ? 0.5f * place.thumb_length : point - place.thumb_begin;
        scrollbar_drag = UIScrollbarDrag{ bar, UIThumbGrab{ offset } };
        DragScrollbar(window_x, window_y);
        return;
    }
    scrollbar_drag = UIScrollbarDrag{ bar, UIPageRepeat{ direction, SDL_GetTicks() + kPageRepeatDelayMs } };
    SetScroll(*view, place, AxisScroll(*view, place.axis) + direction * place.page);
}

void UI_Manager::DragScrollbar(float window_x, float window_y)
{
    constexpr uint64_t kPageRepeatRateMs = 50;

    if (!scrollbar_drag.bar) return;
    UIThumbPlace place;
    UIScrollView* view = DraggableView(scrollbar_drag.bar, place);
    if (!view) { scrollbar_drag = {}; return; }
    const float point = Along(LayoutPoint(scrollbar_drag.bar, window_x, window_y), place.axis);

    if (const UIThumbGrab* grab = std::get_if<UIThumbGrab>(&scrollbar_drag.mode)) {
        const float share = (point - grab->offset - place.track_begin) / (place.track_length - place.thumb_length);
        SetScroll(*view, place, std::clamp(share, 0.0f, 1.0f) * place.range);
    } else {
        UIPageRepeat* repeat = std::get_if<UIPageRepeat>(&scrollbar_drag.mode);
        assert(repeat);
        const uint64_t now = SDL_GetTicks();
        if (now < repeat->next_ms || PageDirection(place, point) != repeat->direction) return;
        repeat->next_ms = now + kPageRepeatRateMs;
        SetScroll(*view, place, AxisScroll(*view, place.axis) + repeat->direction * place.page);
    }
}

void UI_Manager::SetScroll(UIScrollView& view, const UIThumbPlace& place, float scroll)
{
    float& current = AxisScroll(view, place.axis);
    const float clamped = std::clamp(scroll, 0.0f, place.range);
    if (clamped == current) return;
    current     = clamped;
    emit_needed = true;
}

const std::string& UIScrollbarData::ThumbMaterial() const
{
    if (thumb_state == UIThumbState::Grabbed && !thumb_materials.grabbed.empty()) return thumb_materials.grabbed;
    if (thumb_state != UIThumbState::Idle && !thumb_materials.hovered.empty()) return thumb_materials.hovered;
    return thumb_materials.idle;
}

void UI_Manager::UpdateScrollbars(EngineContext* ctx, float window_x, float window_y)
{
    DragScrollbar(window_x, window_y);

    const UINodeIndex hovered = mouse_enabled ? ThumbAt(window_x, window_y) : UINodeIndex{};
    const UINodeIndex grabbed = std::holds_alternative<UIThumbGrab>(scrollbar_drag.mode) ? scrollbar_drag.bar : UINodeIndex{};
    for (size_t index = 0; index < nodes.size(); ++index) {
        UIScrollbarData* scrollbar = std::get_if<UIScrollbarData>(&nodes[index].kind);
        if (!scrollbar) continue;
        const UINodeIndex bar{ safe_u32(index) };
        const UIThumbState state = bar == grabbed ? UIThumbState::Grabbed
                                 : bar == hovered ? UIThumbState::Hovered : UIThumbState::Idle;
        if (state == scrollbar->thumb_state) continue;
        const std::string& previous = scrollbar->ThumbMaterial();
        scrollbar->thumb_state = state;
        if (nodes[index].has_entity && scrollbar->HasThumb() && scrollbar->ThumbMaterial() != previous)
            ctx->ChangeMaterial(scrollbar->thumb_entity, scrollbar->ThumbMaterial());
    }
}

void UI_Manager::UpdateCaret(EngineContext* ctx)
{
    constexpr uint64_t kBlinkPeriodMs  = 1200;
    constexpr uint64_t kBlinkVisibleMs = 800;
    // Впереди всех узлов, но не на ближней плоскости: округление увело бы каретку за неё.
    constexpr float kCaretNdcZ = 0.001f;

    ObjectManager* object_manager = ctx->GetObjectManager();
    SceneData*     scene          = object_manager->GetActiveScene();
    if (!scene) return;
    const SceneName scene_name = object_manager->GetActiveSceneName();

    // Каретку получает только поле с Renderable: у поля без модели caret_entity не задан, и под этим
    // номером может оказаться чужая энтить.
    if (caret_node != focused_node && caret_node && nodes[caret_node.value].has_entity && nodes[caret_node.value].HasRenderable())
        if (const UIInputData* previous = std::get_if<UIInputData>(&nodes[caret_node.value].kind))
            ctx->HideEntity(scene_name, previous->caret_entity, false);
    caret_node = focused_node;
    if (!focused_node) return;

    const UINode& entry = nodes[focused_node.value];
    const UIInputData* input = std::get_if<UIInputData>(&entry.kind);
    if (!input || !entry.has_entity || !entry.HasRenderable() || !object_manager->Has<LocalMatrices>(scene, input->caret_entity))
        return;
    if (entry.layout_width <= 0.0f || entry.layout_height <= 0.0f) return;
    ctx->HideEntity(scene_name, input->caret_entity, true);

    const std::vector<UIGlyphPlace>& places = input->text.glyph_places;
    const UIGlyphPlace& place = places[std::min(focused_prefix + caret, places.size() - 1)];
    const float line_height = input->text.shape->line_height;
    const float width = CaretLength(entry.style.caret_width_mode, entry.style.caret_width, line_height);
    const float gap   = CaretLength(entry.style.caret_gap_mode,   entry.style.caret_gap,   line_height);
    const float left  = std::min(place.x - gap, std::max(entry.layout_width - width, 0.0f));
    const float top  = safe_sint32_f(place.line) * line_height;

    SoAElement<LocalMatrices> local = object_manager->GetComponent<LocalMatrices>(scene, input->caret_entity);
    LocalMatrices& columns = local.container();
    const size_t   row     = local.i();
    columns.m0[row]  = width / entry.layout_width;
    columns.m5[row]  = line_height / entry.layout_height;
    columns.m12[row] = (left + 0.5f * width) / entry.layout_width - 0.5f;
    columns.m13[row] = 0.5f - (top + 0.5f * line_height) / entry.layout_height;
    columns.m14[row] = (kCaretNdcZ - entry.ndc_z) * 0.5f * last_screen_height;

    UITextComponent& text = object_manager->GetComponent<UITextComponent>(scene, input->caret_entity);
    text.glyphs.assign(input->text.wrapped_glyphs.begin() + place.wrapped_index, input->text.wrapped_glyphs.end());
    text.width_lines  = width / line_height;
    text.height_lines = 1.0f;
    text.indent_lines = (place.x - left) / line_height;

    const bool lit = (SDL_GetTicks() - caret_blink_start) % kBlinkPeriodMs < kBlinkVisibleMs;
    SoAElement<Renderable> renderable = object_manager->GetComponent<Renderable>(scene, input->caret_entity);
    renderable.container().alpha[renderable.i()] = lit ? 1.0f : 0.0f;
}

void UI_Manager::ExecuteClicks()
{
    std::vector<UIClick> clicks;
    clicks.swap(pending_clicks);
    for (const UIClick& click : clicks) {
        const auto instruction = click_instructions.find(click.id);
        if (instruction == click_instructions.end()) continue;
        // Копия: функтор может загрузить сцену, а Reset очищает реестр вместе с исполняемым функтором.
        const UIClickFunc click_handler = instruction->second;
        click_handler(click.instance);
    }
}

UIHit UI_Manager::HitAt(float window_x, float window_y) const
{
    UIHit hit;
    if (root) FindHit(root, {}, window_x, window_y, hit);
    return hit;
}

UINodeIndex UI_Manager::ClickableNodeAt(float window_x, float window_y) const
{
    return HitAt(window_x, window_y).clickable;
}

bool UI_Manager::IsOverUI(float window_x, float window_y) const
{
    return bool(HitAt(window_x, window_y).node);
}

void UI_Manager::FindHit(UINodeIndex node, UINodeIndex clickable_ancestor, float window_x, float window_y, UIHit& hit) const
{
    if (!IsNodeShown(node)) return;
    const UINode& entry = nodes[node.value];
    const bool takes_click = click_instructions.contains(entry.id) || std::holds_alternative<UIInputData>(entry.kind);
    const bool visible     = entry.HasRenderable() || std::holds_alternative<UIScrollbarData>(entry.kind);
    const UINodeIndex nearest_clickable = takes_click ? node : clickable_ancestor;

    if (entry.ndc_z < hit.z && (((visible || takes_click) && NodeContains(node, window_x, window_y)) ||
                                (entry.Scrolls() && ContentContains(node, window_x, window_y))))
        hit = UIHit{ entry.ndc_z, node, nearest_clickable };
    for (UINodeIndex child : entry.children)
        FindHit(child, nearest_clickable, window_x, window_y, hit);
}
