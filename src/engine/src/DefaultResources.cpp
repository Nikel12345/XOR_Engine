#include "PCH.h"
#include "DefaultResources.h"
#include "EngineContext.h"
#include "BufferManager.h"          // DefaultBuffersNames + буфер/апдейтер кадра фрактала
#include "CameraManager.h"          // позиция активной камеры — ребейз перенормированного кадра
#include "DefaultRenderPassSet.h"   // MAIN_PASS — оба фона рисуются в main-проходе
#include "PositionStructure.h"      // FMT_PosUVNormal — раскладка вершин skybox_cube

using namespace ShaderBase;   // POSITION в pull-раскладке _skybox_vs

// ============================================================
//  Дефолтные ресурсы с альтернативными наборами (см. заголовок).
//  Фон сцены: общая часть (куб + вершинник от камеры) + два варианта
//  фрагментной половины — env-кубмапа или Мандельброт.
// ============================================================

namespace DefaultResourcesNamespace
{

// Общая часть обоих фонов: куб [-1,1]³ + вершинник, строящий позицию из камерного буфера.
// VS тянет только POSITION, поэтому UV/нормали/тангенсы — нули; winding не важен
// (DoesNotCull, камера всегда внутри).
static void CreateBackgroundCommon(EngineContext* ctx)
{
	ctx->CreateModel("skybox_cube", [](std::vector<PosUVNormal>& v, std::vector<Uint32>& i) {
		v = {
			{ -1,-1,-1,  0,0,  0,0,0,  0,0,0 },
			{  1,-1,-1,  0,0,  0,0,0,  0,0,0 },
			{  1, 1,-1,  0,0,  0,0,0,  0,0,0 },
			{ -1, 1,-1,  0,0,  0,0,0,  0,0,0 },
			{ -1,-1, 1,  0,0,  0,0,0,  0,0,0 },
			{  1,-1, 1,  0,0,  0,0,0,  0,0,0 },
			{  1, 1, 1,  0,0,  0,0,0,  0,0,0 },
			{ -1, 1, 1,  0,0,  0,0,0,  0,0,0 },
		};
		i = {
			0,1,2, 0,2,3,   // -Z
			5,4,7, 5,7,6,   // +Z
			4,0,3, 4,3,7,   // -X
			1,5,6, 1,6,2,   // +X
			3,2,6, 3,6,7,   // +Y
			4,5,1, 4,1,0,   // -Y
		};
	}, AnchorShift::Keep, /*dont_save=*/true);

	ctx->CreateVertexShader("_skybox_vs",
		"../engine/shaders_code/skybox/skybox.vert.hlsl",
		{ { &FMT_PosUVNormal, { POSITION } } }, /*dont_save=*/true);
}

// SPD фона: глубину не пишет, тестит LESS_OR_EQUAL — VS кладёт z=w (глубина ровно 1.0 = клир),
// фон проходит только по пустому депту, любая геометрия перекрывает его независимо от порядка
// отрисовки внутри пасса.
static ShaderProgramDescription BackgroundSpd()
{
	ShaderProgramDescription spd;
	spd.DoesNotCull()->ReadsDepthOnly()->WithDepthCompare(SDL_GPU_COMPAREOP_LESS_OR_EQUAL);
	return spd;
}

void SetDefaultSkyboxResources(EngineContext* ctx)
{
	using namespace DefaultBuffersNames;
	CreateBackgroundCommon(ctx);

	ctx->CreateFragmentShader("_skybox_fs", "../engine/shaders_code/skybox/skybox.frag.hlsl", /*dont_save=*/true);

	// Из vs-буферов нужна только камера; текстур у материала нет — env-кубмапа приходит
	// глобалкой MAIN_PASS (слот 1).
	ctx->CreateShaderProgram("_Skybox", BackgroundSpd(), DefaultRenderPassNamespace::MAIN_PASS,
		"_skybox_vs", { DEFAULT_CAMERA_BUFFER },
		"_skybox_fs", { },
		{ }, /*dont_save=*/true);

	ctx->CreateMaterial("_skybox", {}, { "_Skybox" }, /*dont_save=*/true);
}

// ════════════════════════════════════════════════════════════════════════════
//  Перенормированный кадр фрактала — «4-я координата» позиции (масштаб).
//  Позиция камеры в мире губки = стек символов спуска (целая часть глубины,
//  не переполняется) + локальные float-координаты в кадре ТЕКУЩЕЙ ячейки
//  (всегда O(1) — float счастлив на любой глубине). Ребейз ×3/÷3 держит камеру
//  в «своём» масштабе по гистерезису DE; шейдер видит только локальный кадр +
//  K уровней предков (дальнее поле, глубже прячет туман).
// ════════════════════════════════════════════════════════════════════════════

// Кадр фрактала: [0] = (позиция камеры в локальном кадре, глубина), [1] = (K, 0,0,0),
// [2+j] = (офсет предка j+1 уровней вверх, 3^(j+1)). Dynamic, пишется апдейтером каждый кадр.
static constexpr const char* FRACTAL_FRAME_BUFFER = "_FractalFrameBuffer";
static constexpr uint32_t FRACTAL_MAX_CONTEXT = 8;   // уровней предков в шейдер (дальше — туман)

// Стек спуска: офсеты D перехода в подъячейку (компоненты ∈ {-2,0,2}; p' = 3p - D).
// Живёт на sim-потоке (мутируется только апдейтером кадра), как g_pass_system у пассов.
static std::vector<glm::vec3> g_fractal_path;

// Целевая глубина кадра — РУЧНАЯ (клавиши I/K игры через FractalScaleStep). Авто-ребейз по
// DE выпилен: он менял масштаб (а с ним скорость и детализацию) в непредсказуемые моменты.
// Апдейтер лишь приводит фактическую глубину к целевой и перепривязывает боковые выходы
// из ячейки БЕЗ смены масштаба. Sim-поток (апдейт игры и prepare — один поток, без гонок).
static int g_fractal_target_depth = 0;

void FractalScaleStep(int delta)
{
	g_fractal_target_depth = std::max(0, g_fractal_target_depth + delta);
	SDL_Log("Fractal scale: target depth %d", g_fractal_target_depth);
}

void SetFractalSkyboxResources(EngineContext* ctx)
{
	using namespace DefaultBuffersNames;

	// 3D-фрактал рисуется тем же путём, что классический скайбокс: куб вокруг камеры +
	// _skybox_vs (v_dir — мировое направление луча пикселя, глубина ровно 1.0). Различие —
	// только фрагментник: рэймарч губки Менгера по её замкнутой функции расстояния.
	CreateBackgroundCommon(ctx);

	ctx->CreateFragmentShader("_fractal_fs", "../engine/shaders_code/fractal/menger.frag.hlsl", /*dont_save=*/true);

	// ── Кадр фрактала: апдейтер делает ребейз камеры и публикует локальный кадр слоту.
	// Порядок регистрации важен ровно наоборот, чем кажется: камерный апдейтер уже снял
	// view ДО нас (регистрировался раньше), но из view шейдер фрактала берёт ТОЛЬКО
	// ротацию (v_dir) — она ребейзом не меняется; позиция луча идёт из ЭТОГО буфера,
	// консистентного с пост-ребейзным состоянием. ──
	{
		BufferManager* bm = ctx->GetBufferManager();
		CameraManager* cm = ctx->GetCameraManager();
		const uint32_t frame_bytes = (2 + FRACTAL_MAX_CONTEXT) * 4 * sizeof(float);

		bm->CreateBufferData(FRACTAL_FRAME_BUFFER, frame_bytes,
			SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ, BufferDataType::Dynamic);

		bm->CreateUpdateInstruction(FRACTAL_FRAME_BUFFER,
			[cm](SDL_GPUCopyPass*, BufferManager* bm, UploadTask& task)
		{
			float* out = static_cast<float*>(bm->AcquireTransferWritePtr(&task,
				(2 + FRACTAL_MAX_CONTEXT) * 4 * sizeof(float)));
			if (!out) return;

			glm::vec3 p(0.0f);
			if (Camera* cam = cm->GetActiveCamera()) {
				p = cam->GetPosition();

				// Приведение кадра к РУЧНОЙ целевой глубине + боковая перепривязка. Ребейз —
				// точная замена координат (та же мировая точка), картинку не меняет; масштаб
				// меняется ТОЛЬКО по явной команде игрока (I/K). Боковой выход из ячейки
				// (|p|>1 на глубине) чинится парой подъём→спуск-в-соседа — итог без смены
				// масштаба, скорость не прыгает. Не больше 8 шагов за тик.
				for (int guard = 0; guard < 8; ++guard) {
					const int depth = (int)g_fractal_path.size();
					const float amax = std::max(std::abs(p.x), std::max(std::abs(p.y), std::abs(p.z)));
					if (depth > 0 && (depth > g_fractal_target_depth || amax > 1.0f)) {
						// Лишняя глубина ИЛИ боковой выход — подъём (выход тут же вернётся
						// спуском в содержащего соседа веткой ниже: depth стал < target).
						const glm::vec3 D = g_fractal_path.back();
						g_fractal_path.pop_back();
						p = (p + D) / 3.0f;
					}
					else if (depth < g_fractal_target_depth && amax <= 1.0f) {
						// Спуск в подъячейку, содержащую камеру. Вне ячейки (напр. снаружи
						// корня) спуск невозможен — глубина догонит цель после входа.
						glm::vec3 D;
						for (int a = 0; a < 3; ++a) {
							const int idx = std::clamp((int)std::floor((p[a] + 1.0f) * 1.5f), 0, 2);
							D[a] = 2.0f * (float)(idx - 1);
						}
						g_fractal_path.push_back(D);
						p = 3.0f * p - D;
					}
					else break;
				}
				cam->SetPosition(p);   // движение продолжается в текущем кадре (скорость = локальная)
			}

			// Публикация кадра слоту: позиция+глубина, K, офсеты предков (см. раскладку выше).
			const int depth = (int)g_fractal_path.size();
			const int K = std::min(depth, (int)FRACTAL_MAX_CONTEXT);
			out[0] = p.x; out[1] = p.y; out[2] = p.z; out[3] = (float)depth;
			out[4] = (float)K; out[5] = out[6] = out[7] = 0.0f;
			glm::vec3 o(0.0f);
			float s = 1.0f;
			for (int j = 1; j <= (int)FRACTAL_MAX_CONTEXT; ++j) {
				if (j <= K) {
					o = (o + g_fractal_path[depth - j]) / 3.0f;
					s *= 3.0f;
				}
				float* e = out + (1 + j) * 4;
				e[0] = o.x; e[1] = o.y; e[2] = o.z; e[3] = s;
			}
		},
			[]() -> uint32_t { return (2 + FRACTAL_MAX_CONTEXT) * 4 * sizeof(float); });
	}

	// Фрагментнику камерный буфер НЕ передаётся: origin луча — из кадра фрактала (t2,
	// единственный fs-буфер), ротация уходит в v_dir ещё в VS. Неиспользуемое объявление
	// камеры DXC стрипал бы → дырка в слотах storage-буферов → пайплайн не собирается
	// (SDL ждёт плотные слоты — та же грабля, что якоря сэмплеров). Push-константы
	// (FractalPushData) биндит игра ПОЛЕМ push_func (см. заголовок).
	ctx->CreateShaderProgram("_Fractal", BackgroundSpd(), DefaultRenderPassNamespace::MAIN_PASS,
		"_skybox_vs", { DEFAULT_CAMERA_BUFFER },
		"_fractal_fs", { FRACTAL_FRAME_BUFFER },
		{ }, /*dont_save=*/true);

	// Материал НАМЕРЕННО под общим именем "_skybox" — его по имени подхватывает
	// скайбокс-энтити каждого LoadScene (см. заголовок).
	ctx->CreateMaterial("_skybox", {}, { "_Fractal" }, /*dont_save=*/true);
}

}
