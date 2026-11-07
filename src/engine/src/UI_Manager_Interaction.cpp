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
    if (focused_node != kInvalid) caret = CaretAt(window_x, window_y);
}

void UI_Manager::ReleaseAt(float window_x, float window_y)
{
    const Node released_node = ClickableNodeAt(window_x, window_y);
    if (released_node != kInvalid && released_node == pressed_node)
        pending_clicks.push_back({ nodes[released_node].id, nodes[released_node].instance });
    CancelPress();
}

void UI_Manager::CancelPress()
{
    press_on_ui  = false;
    pressed_node = kInvalid;
}

void UI_Manager::SetMouseEnabled(bool enabled)
{
    mouse_enabled = enabled;
    if (!enabled) CancelPress();
}

bool UI_Manager::WantCaptureMouse(float window_x, float window_y) const
{
    if (!mouse_enabled) return false;
    return press_on_ui || focused_node != kInvalid || IsOverUI(window_x, window_y);
}

void UI_Manager::SetFocus(Node node)
{
    focused_node = InputVarOf(node, focused_var, focused_key) ? node : kInvalid;
    if (focused_node == kInvalid) return;
    focused_prefix    = InputPrefixLength(node);
    caret_blink_start = SDL_GetTicks();
}

size_t UI_Manager::InputPrefixLength(Node node) const
{
    size_t length = 0;
    for (const UITemplatePart& part : elements[nodes[node].element].text) {
        length += CharacterCount(part.literal);
        if (part.var) break;
        if (part.loop >= 0) length += CharacterCount(PartValue(part, nodes[node].instance));
    }
    return length;
}

size_t UI_Manager::CaretAt(float window_x, float window_y) const
{
    const UINode& entry = nodes[focused_node];
    const std::vector<UIGlyphPlace>& places = entry.glyph_places;
    const glm::vec2 point = NodePoint(focused_node, window_x, window_y);
    const float x = point.x + 0.5f * entry.layout_width;
    const float y = 0.5f * entry.layout_height - point.y;
    const int line = std::clamp(safe_sint32(y / entry.shape->line_height), 0, places.back().line);

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
    focused_node = kInvalid;
}

bool UI_Manager::WantCaptureKeyboard() const
{
    return focused_node != kInvalid;
}

void UI_Manager::HandleInput(const std::vector<UIInputEvent>& events)
{
    if (focused_node != kInvalid && !IsNodeShown(focused_node)) ClearFocus();
    if (focused_node == kInvalid) return;

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

    if (caret_node != focused_node && caret_node != kInvalid && nodes[caret_node].has_entity)
        ctx->HideEntity(scene_name, nodes[caret_node].caret_entity, false);
    caret_node = focused_node;
    if (focused_node == kInvalid) return;

    const UINode& entry = nodes[focused_node];
    if (!entry.has_entity || !object_manager->Has<LocalMatrices>(scene, entry.caret_entity)) return;
    if (entry.layout_width <= 0.0f || entry.layout_height <= 0.0f) return;
    ctx->HideEntity(scene_name, entry.caret_entity, true);

    const UIGlyphPlace& place = entry.glyph_places[std::min(focused_prefix + caret, entry.glyph_places.size() - 1)];
    const float line_height = entry.shape->line_height;
    const float width = CaretLength(entry.style.caret_width_mode, entry.style.caret_width, line_height);
    const float gap   = CaretLength(entry.style.caret_gap_mode,   entry.style.caret_gap,   line_height);
    const float left  = std::min(place.x - gap, std::max(entry.layout_width - width, 0.0f));
    const float top  = safe_sint32_f(place.line) * line_height;

    SoAElement<LocalMatrices> local = object_manager->GetComponent<LocalMatrices>(scene, entry.caret_entity);
    LocalMatrices& columns = local.container();
    const size_t   row     = local.i();
    columns.m0[row]  = width / entry.layout_width;
    columns.m5[row]  = line_height / entry.layout_height;
    columns.m12[row] = (left + 0.5f * width) / entry.layout_width - 0.5f;
    columns.m13[row] = 0.5f - (top + 0.5f * line_height) / entry.layout_height;
    columns.m14[row] = (kCaretNdcZ - entry.ndc_z) * 0.5f * last_screen_height;

    UITextComponent& text = object_manager->GetComponent<UITextComponent>(scene, entry.caret_entity);
    text.glyphs.assign(entry.wrapped_glyphs.begin() + place.wrapped_index, entry.wrapped_glyphs.end());
    text.width_lines  = width / line_height;
    text.height_lines = 1.0f;
    text.indent_lines = (place.x - left) / line_height;

    const bool lit = (SDL_GetTicks() - caret_blink_start) % kBlinkPeriodMs < kBlinkVisibleMs;
    SoAElement<Renderable> renderable = object_manager->GetComponent<Renderable>(scene, entry.caret_entity);
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

UI_Manager::Node UI_Manager::ClickableNodeAt(float window_x, float window_y) const
{
    if (root == kInvalid) return kInvalid;
    float top_z     = std::numeric_limits<float>::max();
    Node  clickable = kInvalid;
    FindClickableNode(root, kInvalid, window_x, window_y, top_z, clickable);
    return clickable;
}

bool UI_Manager::IsOverUI(float window_x, float window_y) const
{
    if (root == kInvalid) return false;
    constexpr float kNoHit = std::numeric_limits<float>::max();
    float top_z     = kNoHit;
    Node  clickable = kInvalid;
    FindClickableNode(root, kInvalid, window_x, window_y, top_z, clickable);
    return top_z != kNoHit;
}

void UI_Manager::FindClickableNode(Node node, Node clickable_ancestor, float window_x, float window_y,
                                   float& top_z, Node& clickable) const
{
    if (!IsNodeShown(node)) return;
    const UINode& entry = nodes[node];
    const bool takes_click       = click_instructions.contains(entry.id) || IsInputNode(node);
    const bool visible           = entry.draws && !entry.quad.empty();
    const Node nearest_clickable = takes_click ? node : clickable_ancestor;

    if ((visible || takes_click) && entry.ndc_z < top_z && NodeContains(node, window_x, window_y)) {
        top_z     = entry.ndc_z;
        clickable = nearest_clickable;
    }
    for (Node child : entry.children)
        FindClickableNode(child, nearest_clickable, window_x, window_y, top_z, clickable);
}
