#include "PCH.h"
#include "UI_Manager.h"
#include "EngineContext.h"
#include "ObjectManager.h"
#include "BaseComponents.h"
#include <algorithm>
#include <limits>

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

void UI_Manager::PressAt(float window_x, float window_y)
{
    if (!mouse_enabled) return;
    press_on_ui  = IsOverUI(window_x, window_y);
    pressed_node = ClickableNodeAt(window_x, window_y);
    SetFocus(pressed_node);
    if (focused_node) caret = CaretAt(window_x, window_y);
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
    press_on_ui  = false;
    pressed_node = {};
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
    if (focused_node && !IsNodeShown(focused_node)) ClearFocus();
    if (!focused_node) return;

    for (const UIInputEvent& event : events) {
        caret_blink_start = SDL_GetTicks();
        std::string value = GetVar(focused_var, focused_key);
        caret = std::min(caret, CharacterCount(value));
        if (event.kind == UIInputKind::Text) {
            value.insert(ByteOffset(value, caret), event.text);
            caret += CharacterCount(event.text);
        } else switch (event.scancode) {
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

    if (caret_node != focused_node && caret_node && nodes[caret_node.value].has_entity)
        if (const UIInputData* previous = std::get_if<UIInputData>(&nodes[caret_node.value].kind))
            ctx->HideEntity(scene_name, previous->caret_entity, false);
    caret_node = focused_node;
    if (!focused_node) return;

    const UINode& entry = nodes[focused_node.value];
    const UIInputData* input = std::get_if<UIInputData>(&entry.kind);
    if (!input || !entry.has_entity || !object_manager->Has<LocalMatrices>(scene, input->caret_entity)) return;
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

UINodeIndex UI_Manager::ClickableNodeAt(float window_x, float window_y) const
{
    if (!root) return {};
    float top_z     = std::numeric_limits<float>::max();
    UINodeIndex clickable;
    FindClickableNode(root, {}, window_x, window_y, top_z, clickable);
    return clickable;
}

bool UI_Manager::IsOverUI(float window_x, float window_y) const
{
    if (!root) return false;
    constexpr float kNoHit = std::numeric_limits<float>::max();
    float top_z     = kNoHit;
    UINodeIndex clickable;
    FindClickableNode(root, {}, window_x, window_y, top_z, clickable);
    return top_z != kNoHit;
}

void UI_Manager::FindClickableNode(UINodeIndex node, UINodeIndex clickable_ancestor, float window_x, float window_y,
                                   float& top_z, UINodeIndex& clickable) const
{
    if (!IsNodeShown(node)) return;
    const UINode& entry = nodes[node.value];
    const bool takes_click       = click_instructions.contains(entry.id) || std::holds_alternative<UIInputData>(entry.kind);
    const bool visible           = entry.draws && !entry.quad.empty();
    const UINodeIndex nearest_clickable = takes_click ? node : clickable_ancestor;

    if ((visible || takes_click) && entry.ndc_z < top_z && NodeContains(node, window_x, window_y)) {
        top_z     = entry.ndc_z;
        clickable = nearest_clickable;
    }
    for (UINodeIndex child : entry.children)
        FindClickableNode(child, nearest_clickable, window_x, window_y, top_z, clickable);
}
