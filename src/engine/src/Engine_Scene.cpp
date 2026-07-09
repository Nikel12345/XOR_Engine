#include "PCH.h"
#include "Engine.h"
#include "EngineProfiler.h"
#include <fstream>      // Save/LoadScene: файлы сцены-папки
#include <filesystem>   // SaveScene: create_directories папки сцены
#include "yyjson.h"     // json-манифесты ресурсов сцены (textures.json, ...)

// ============================================================
//  Сцена-папка: scene.scene (ECS) + файлы ресурсов рядом (json, по менеджерам — поэтапно).
//  Публичная точка входа — ctx->Save/LoadScene (тонкий прокси сюда).
// ============================================================

// ChannelConvention ↔ строка манифеста (в json — читаемое имя, не число).
static const char* ConvToStr(ChannelConvention c)
{
	switch (c) {
	case ChannelConvention::SmoothnessInGreen: return "SmoothnessInGreen";
	case ChannelConvention::DepthInAlpha:      return "DepthInAlpha";
	default:                                   return "AsIs";
	}
}
static ChannelConvention ConvFromStr(const char* s)
{
	if (s && std::strcmp(s, "SmoothnessInGreen") == 0) return ChannelConvention::SmoothnessInGreen;
	if (s && std::strcmp(s, "DepthInAlpha") == 0)      return ChannelConvention::DepthInAlpha;
	return ChannelConvention::AsIs;
}
// Строковое поле json-объекта; отсутствие/не-строка → пустая строка (валидность решает потребитель).
static const char* JsonStr(yyjson_val* obj, const char* key)
{
	const char* s = yyjson_get_str(yyjson_obj_get(obj, key));
	return s ? s : "";
}

void Engine::SaveScene(const SceneName& scene_name, const std::string& dir)
{
	SceneData* scene = object_manager->GetScene(scene_name);
	if (!scene) { SDL_Log("SaveScene: scene '%s' not found", scene_name.c_str()); return; }

	std::error_code ec;
	std::filesystem::create_directories(dir, ec);   // папка сцены (уже существует — не ошибка)
	if (ec) { SDL_Log("SaveScene: cannot create dir '%s' (%s)", dir.c_str(), ec.message().c_str()); return; }

	// ── ECS → scene.scene (как раньше, формат om не меняем) ──
	{
		const std::string text = object_manager->SaveScene(scene);
		const std::string path = dir + "/scene.scene";
		std::ofstream f(path, std::ios::binary);
		if (!f) { SDL_Log("SaveScene: cannot open '%s' for write", path.c_str()); return; }
		f << text;
	}

	// ── Текстуры → textures.json (merge-манифест). Скип: dont_save (движковые дефолты) и
	//    байтовые (пустой source_path — из файла не пересоздаются, их делает код). ──
	{
		yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
		yyjson_mut_val* root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);
		yyjson_mut_val* arr = yyjson_mut_obj_add_arr(doc, root, "textures");
		size_t saved = 0;
		for (auto& [name, h] : texture_manager->GetTextureHandles()) {
			if (!h || h->dont_save || h->source_path.empty()) continue;
			yyjson_mut_val* t = yyjson_mut_arr_add_obj(doc, arr);
			yyjson_mut_obj_add_strcpy(doc, t, "name",  name.c_str());
			yyjson_mut_obj_add_strcpy(doc, t, "atlas", h->atlas_name.c_str());
			yyjson_mut_obj_add_strcpy(doc, t, "path",  h->source_path.c_str());
			yyjson_mut_obj_add_str   (doc, t, "conv",  ConvToStr(h->conv));   // статический литерал — без копии
			++saved;
		}
		yyjson_write_err werr;
		const std::string path = dir + "/textures.json";
		if (!yyjson_mut_write_file(path.c_str(), doc, YYJSON_WRITE_PRETTY, nullptr, &werr))
			SDL_Log("SaveScene: cannot write '%s' (%s)", path.c_str(), werr.msg);
		else
			SDL_Log("SaveScene: %zu textures -> textures.json", saved);
		yyjson_mut_doc_free(doc);
	}

	SDL_Log("SaveScene: wrote scene '%s' to '%s'", scene_name.c_str(), dir.c_str());
}

void Engine::LoadScene(const SceneName& scene_name, const std::string& dir)
{
	// ── Тайминг фаз загрузки (диагностика 5-секундной загрузки 100k). Load — событие
	// разовое, поэтому не через кадровый Prof, а прямым SDL_Log сразу после. ──
	const auto t_read = Prof::Clock::now();
	const std::string scene_path = dir + "/scene.scene";
	std::ifstream f(scene_path, std::ios::binary | std::ios::ate);   // ate: сразу в конец — узнать размер
	if (!f) { SDL_Log("LoadScene: cannot open '%s'", scene_path.c_str()); return; }
	// Читаем ФАЙЛ ОДНОЙ аллокацией прямо в строку (без stringstream → без тройной копии
	// 43 МБ: rdbuf-буфер + ss.str()). Одна аллокация точного размера + один read.
	const std::streamoff sz = f.tellg();
	std::string text;
	if (sz > 0) {
		text.resize(static_cast<size_t>(sz));
		f.seekg(0);
		f.read(text.data(), sz);
		text.resize(static_cast<size_t>(f.gcount()));          // усечь до реально прочитанного
	}
	const double read_ms = Prof::MsSince(t_read);

	// ── Ресурсы ПЕРЕД ECS (merge-upsert; фикс-ап указателей сущностей резолвит по словарям).
	//    Текстуры: json-манифест → разобранный список → tm (словарная семантика — его, декод
	//    файла — колбэком через ctx). Отсутствие файла — валидная частичная папка, только лог.
	//    Дальше по этапам: модели/материалы/шейдеры. ──
	double tex_ms = 0.0;
	{
		const auto t_tex = Prof::Clock::now();
		const std::string tex_path = dir + "/textures.json";
		yyjson_read_err rerr;
		if (yyjson_doc* doc = yyjson_read_file(tex_path.c_str(), 0, nullptr, &rerr)) {
			std::vector<SceneTextureEntry> entries;
			yyjson_val* arr = yyjson_obj_get(yyjson_doc_get_root(doc), "textures");
			entries.reserve(yyjson_arr_size(arr));
			size_t idx, max; yyjson_val* t;
			yyjson_arr_foreach(arr, idx, max, t) {
				entries.push_back({ JsonStr(t, "name"), JsonStr(t, "atlas"), JsonStr(t, "path"),
				                    ConvFromStr(JsonStr(t, "conv")) });
			}
			yyjson_doc_free(doc);

			const size_t created = texture_manager->LoadSceneTextures(entries,
				[this](const SceneTextureEntry& e) {
					return engine_context->CreateTextureFromFile(e.name, e.atlas, e.path.c_str(), e.conv);
				});
			SDL_Log("LoadScene: %zu/%zu textures from manifest", created, entries.size());
		}
		else SDL_Log("LoadScene: no textures.json ('%s') — skipped", rerr.msg);
		tex_ms = Prof::MsSince(t_tex);
	}

	// Replace-on-load: сносим прежнее содержимое сцены ДО наполнения — иначе загрузка
	// дописала бы поверх (дубликаты сущностей). Делаем это только после успешного
	// открытия файла, чтобы кривой путь не обнулял текущую сцену.
	//
	// Замок на ECS-swap не нужен: рендер-проходы/каллинг читают пер-слотовые слепки, а не
	// ECS. Единственный живой читатель ECS на рендер-потоке — UI (осознанный компромисс,
	// см. Engine::RenderFunc; правильное закрытие — UI-слепок или построение UI в sim).
	double clear_ms = 0.0, ecs_ms = 0.0, ptr_ms = 0.0;
	std::vector<Entity> loaded;
	{
		const auto t_clear = Prof::Clock::now();
		if (SceneData* prev = object_manager->GetScene(scene_name)) prev->clear();
		clear_ms = Prof::MsSince(t_clear);

		// ECS-часть: текст → сущности (указатели на ассеты пока пустые, только имена).
		// Возвращает ИМЕННО созданные этой загрузкой сущности.
		const auto t_ecs = Prof::Clock::now();
		loaded = object_manager->LoadScene(scene_name, text);
		ecs_ms = Prof::MsSince(t_ecs);

		SceneData* scene = object_manager->GetScene(scene_name);
		if (scene) {
			// Чиним указатели на ассеты по сохранённым именам — это знает только верхний слой.
			// Обходим ТОЛЬКО загруженные сущности (не всю сцену): уже существовавшие в ней
			// производные сущности (напр. дебаг-коллайдеры) держат живые указатели без имён —
			// их трогать нельзя. Незаполнённое/неизвестное имя оставляет nullptr; сборщик батчей
			// такие сущности пропускает (см. BatchBuilder::AddEntityToBatches), но логируем.
			const auto t_ptr = Prof::Clock::now();
			for (Entity e : loaded) {
				if (object_manager->Has<ModelComponent>(scene, e)) {
					ModelComponent& m = object_manager->GetComponent<ModelComponent>(scene, e);
					m.model = m.name.empty() ? nullptr : (*model_manager)[m.name];
					if (!m.model)
						SDL_Log("LoadScene: model '%s' not resolved (entity will not render)", m.name.c_str());
				}
				if (object_manager->Has<MaterialComponent>(scene, e)) {
					MaterialComponent& mc = object_manager->GetComponent<MaterialComponent>(scene, e);
					mc.materials.clear();
					mc.materials.reserve(mc.names.size());
					for (const auto& n : mc.names) {
						Material* mat = material_manager->GetMaterial(n);
						if (!mat) SDL_Log("LoadScene: material '%s' not resolved", n.c_str());
						mc.materials.push_back(mat);
					}
				}
			}
			ptr_ms = Prof::MsSince(t_ptr);

			object_manager->SetSceneState(scene_name, true);
		}
	}

	batch_builder->SetDirtyBatches(true);
	SDL_Log("LoadScene: loaded scene '%s' from '%s'", scene_name.c_str(), dir.c_str());
	SDL_Log("LoadScene TIMING [%zu ent, %.1f MB]: read=%.1f  tex=%.1f  clear=%.1f  ecs=%.1f  ptr_restore=%.1f  | total=%.1f ms",
		loaded.size(), text.size() / (1024.0 * 1024.0),
		read_ms, tex_ms, clear_ms, ecs_ms, ptr_ms, read_ms + tex_ms + clear_ms + ecs_ms + ptr_ms);
}
