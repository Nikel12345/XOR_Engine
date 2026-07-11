#include "PCH.h"
#include "FractalBackground.h"
#include "EngineContext.h"
#include "BufferManager.h"   // буфер/апдейтер кадра фрактала
#include "CameraManager.h"   // камера-аккумулятор сдвига (игровые юниты)

// ════════════════════════════════════════════════════════════════════════════
//  Позиция во фрактале = целочисленный адрес ячейки + double-локаль + σ (см.
//  заголовок). Точность НЕ зависит от глубины: локаль всегда ~O(1) (младшие
//  биты double при деле — дельта камеры не поглощается на любой скорости и
//  глубине), адрес — точная целочисленная арифметика (боковой перенос разряда
//  любой длины за тик, без float-цепочек и промежуточных публикаций). Камера
//  движка — аккумулятор сдвига в игровых юнитах: скорость от масштаба не
//  зависит, мировой масштаб применяется при внесении сдвига в локаль.
// ════════════════════════════════════════════════════════════════════════════

namespace FractalBackground
{

// Кадр фрактала: [0] = (позиция камеры в локальном кадре, глубина d), [1] = (K, tFar, 1/туман, 0),
// [2+j] = (офсет предка j+1 уровней вверх, 3^(j+1)). Dynamic, пишется апдейтером каждый кадр.
// tFar/туман — в ЛОКАЛЬНЫХ юнитах, но ∝σ, т.е. непрерывны по s: смена якоря не даёт шва.
static constexpr uint32_t FRACTAL_MAX_CONTEXT = 8;    // уровней предков в шейдер (дальше — туман)
// Потолок глубины — здравый смысл, не точность (адрес целочисленный, локаль всегда ~1).
// 512 уровней = зум 10^244; ограничивает снизу pow(3,-d) в FoldUpToRoot (жив до ~640).
static constexpr int      MAX_DEPTH_LEVELS    = 512;
static constexpr double   MAX_OUT_SIGMA       = 27.0; // σ на корне (d=0): «камера крупнее корня»
// Дальность насыщения тумана в юнитах σ=1. Марш дальше бессмыслен: и хит, и промах дают
// чистый sky — поэтому она же уходит в шейдер как tFar.
static constexpr double   FOG_DIST            = 2500.0;
// Игровая дельта камеры → юниты ячейки: локальный шаг = дельта · BASE_STEP · σ
// (мировой = дельта · BASE_STEP · s — скорость следует за масштабом при постоянной камере).
static constexpr double   BASE_STEP           = 0.05;

// ── Авто-масштаб: «размер камеры» следует за расстоянием до поверхности (приём фрактальных
// леталок — Marble Marcher, Fragmentarium: скорость/шаг из функции расстояния). Подлёт сам
// замедляет полёт (мир = дельта·0.05·s, s ∝ дистанция) и раскрывает детализацию — врезаться
// невозможно, «зум» = просто лети к грани; клавиши масштаба не нужны. ──
static constexpr double   AUTO_SCALE      = 0.5;    // размер камеры = доля расстояния до стенки
static constexpr double   SIGMA_SMOOTH    = 0.15;   // доля лог-разрыва до цели, закрываемая за тик
static constexpr double   MAX_SIGMA_STEP  = 0.22;   // потолок |Δ ln σ| за тик (~0.2 уровня):
                                                    // дистанция в тоннелях пульсирует — окно не дёргаем
static constexpr double   MANUAL_MULT_MIN = 1.0 / 729.0;   // ручной сдвиг окна (I/K): ±6 уровней
static constexpr double   MANUAL_MULT_MAX = 729.0;
// «Оболочка» у потолка глубины: когда якорь упёрся в DepthCapFromAddress, мир больше не
// мельчает вместе с камерой, и зенон-сближение схлопнуло бы локальную дистанцию в ноль за
// секунду — экран заполняет неразрешимая окном структура (чёрные поры/полосы из потолочных
// лучей) и обваливается FPS. Вместо этого держим камеру на этой дистанции от стенки:
// скольжение вдоль — свободно, в стену — выталкивание по градиенту DE.
static constexpr double   MIN_WALL_DIST   = 0.3;

// Состояние (всё на sim-потоке: ввод игры и prepare — один поток, гонок нет).
static std::vector<glm::ivec3> g_addr;          // адрес якоря: цифры {0,1,2} по осям, root-first
static glm::dvec3              g_local(0.0);    // позиция в юнитах якорной ячейки ([-1,1] внутри)
static double                  g_sigma = 1.0;   // дробный масштаб σ; s = σ·3^-depth

static double g_step_scale = BASE_STEP;   // BASE_STEP·σ прошлого кадра — перевод дельты камеры
static double g_manual_mult = 1.0;        // ручной сдвиг окна масштаба (I/K) поверх авто
static double g_prev_dist = 1e9;          // дистанция прошлого тика — потолок шага (анти-туннель)
static bool   g_seeded = false;           // первый тик: позиция камеры — сид, не дельта

void FractalScaleStep(float delta_levels)
{
	// РУЧНОЙ СДВИГ окна относительно авто-масштаба (сам масштаб следует за расстоянием до
	// поверхности): + = «я мельче» (окно глубже), − = крупнее. Нейтраль 1, диапазон ±6 уровней.
	g_manual_mult = std::clamp(g_manual_mult * std::pow(3.0, -(double)delta_levels),
		MANUAL_MULT_MIN, MANUAL_MULT_MAX);
}

// GLSL-мод (floor-периодический), покомпонентно — как glmod в шейдере.
static glm::dvec3 GlmodD(const glm::dvec3& x, double y)
{
	return x - y * glm::floor(x / y);
}

// Дистанция до губки в ЛОКАЛЬНЫХ юнитах кадра — CPU-зеркало шейдерного MengerDE
// (menger.frag.hlsl): бокс предка K уровней вверх + кресты предков из адреса + 12 своих
// уровней периодическим фолдингом. Одна точка на тик — цена пренебрежима.
static double SpongeDistanceLocal(const glm::dvec3& p)
{
	const int depth = (int)g_addr.size();
	const int K = std::min(depth, (int)FRACTAL_MAX_CONTEXT);

	// Кресты предков (вырезают тоннели крупных уровней) + внешний бокс контекста.
	glm::dvec3 o(0.0);
	double sc = 1.0;
	double crosses = -1e300;
	for (int j = 1; j <= K; ++j) {
		o = (o + (glm::dvec3(g_addr[depth - j]) * 2.0 - 2.0)) / 3.0;
		sc *= 3.0;
		const glm::dvec3 a = GlmodD(p / sc + o, 2.0) - 1.0;
		const glm::dvec3 r = glm::abs(1.0 - 3.0 * glm::abs(a));
		const double da = std::max(r.x, r.y), db = std::max(r.y, r.z), dc = std::max(r.z, r.x);
		crosses = std::max(crosses, (std::min(da, std::min(db, dc)) - 1.0) / 3.0 * sc);
	}
	const glm::dvec3 bq = glm::abs(p / sc + o) - 1.0;
	double dist = (glm::length(glm::max(bq, glm::dvec3(0.0)))
		+ std::min(std::max(bq.x, std::max(bq.y, bq.z)), 0.0)) * sc;
	if (K > 0) dist = std::max(dist, crosses);

	// Свои уровни (текущая ячейка и мельче).
	double s = 1.0;
	for (int m = 0; m < 12; ++m) {
		const glm::dvec3 a = GlmodD(p * s, 2.0) - 1.0;
		s *= 3.0;
		const glm::dvec3 r = glm::abs(1.0 - 3.0 * glm::abs(a));
		const double da = std::max(r.x, r.y), db = std::max(r.y, r.z), dc = std::max(r.z, r.x);
		dist = std::max(dist, (std::min(da, std::min(db, dc)) - 1.0) / s);
	}
	return dist;
}

// Сдвиг адреса вдоль оси на n ячеек: позиционное сложение по основанию 3 от глубокой цифры
// к корню (n любой величины — один проход). false = перенос вышел за корень (камера покинула
// корневой куб на глубине) — адрес не тронут, вызывающий сворачивает его в корневую локаль.
static bool ShiftAddrAxis(int axis, long long n)
{
	const int d = (int)g_addr.size();
	int backup[MAX_DEPTH_LEVELS];
	for (int i = 0; i < d; ++i) backup[i] = g_addr[i][axis];

	long long carry = n;
	for (int i = d - 1; i >= 0 && carry != 0; --i) {
		const long long t = (long long)g_addr[i][axis] + carry;
		long long m = t % 3; if (m < 0) m += 3;
		g_addr[i][axis] = (int)m;
		carry = (t - m) / 3;
	}
	if (carry != 0) {
		for (int i = 0; i < d; ++i) g_addr[i][axis] = backup[i];
		return false;
	}
	return true;
}

// Максимум |компоненты| — «насколько камера ушла от якорной ячейки» в её юнитах.
static double AbsMax3(const glm::dvec3& v)
{
	return std::max(std::abs(v.x), std::max(std::abs(v.y), std::abs(v.z)));
}

// Свернуть адрес в корневую локаль (вылет из корня на глубине: тонкая позиция дальше не
// нужна — камера физически покинула фрактал; при возвращении спуск восстановит глубину).
static void FoldUpToRoot()
{
	glm::dvec3 q = g_local;
	for (int i = (int)g_addr.size() - 1; i >= 0; --i) {
		const glm::dvec3 D = glm::dvec3(g_addr[i]) * 2.0 - 2.0;   // idx{0,1,2} → D{-2,0,2}
		q = (q + D) / 3.0;
	}
	g_sigma *= std::pow(3.0, -(double)g_addr.size());   // s неизменен: σ·3^-d → σ' при d=0
	g_local = q;
	g_addr.clear();
	SDL_Log("Fractal: left the root cube at depth — folded to root frame");
}

// Потолок глубины якоря ОТ АДРЕСА — фикс «телепорта в стену» при бесконечном подлёте к грани.
// Контекст шейдера и CPU-DE — только K=8 предков: крест, вырезающий пустоту, в которой ЛЕТИТ
// КАМЕРА (самая МЕЛКАЯ цифра адреса с >=2 компонентами == 1 — «крестовая» подъячейка), обязан
// оставаться в этом окне: depth <= уровень_креста + K. Стоит углубиться дальше — вырез выпадает
// из контекста, и пустоту вокруг камеры кадр рендерит ФАНТОМНОЙ сплошной губкой: экран мгновенно
// «в стене» (выглядит телепортом), CPU-DE у камеры становится <=0, контроллер масштаба пикирует
// на дно — состояние не восстанавливается движением назад. Висеть у грани тоннеля уровня i можно
// с деталью до ~i+K+12 уровней; нырок в дырку стенки делает крестовый уровень мельче и снимает
// потолок — «бесконечность» идёт через полости, как у настоящей губки.
static int DepthCapFromAddress()
{
	const int d = (int)g_addr.size();
	for (int i = 0; i < d; ++i) {
		const glm::ivec3& t = g_addr[i];
		const int ones = (t.x == 1) + (t.y == 1) + (t.z == 1);
		if (ones >= 2) return i + (int)FRACTAL_MAX_CONTEXT;
	}
	return MAX_DEPTH_LEVELS;
}

void CreateFractalFrameResources(EngineContext* ctx)
{
	// ── Кадр фрактала: апдейтер публикует локальный кадр слоту. Порядок регистрации важен
	// ровно наоборот, чем кажется: камерный апдейтер уже снял view ДО нас (регистрировался
	// раньше), но из view шейдер фрактала берёт ТОЛЬКО ротацию (v_dir) — от кадра она не
	// зависит; позиция луча идёт из ЭТОГО буфера. ──
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

		// (1) Сдвиг камеры за тик (аккумулятор в игровых юнитах, обнулён в прошлый тик;
		// при frame-skip копится несколько тиков — кадр между ними не менялся, дельта верна)
		// → в локаль якоря с масштабом ПРОШЛОГО кадра. Обнуляем аккумулятор обратно:
		// приращение к нулю держит полную относительную точность float на любой скорости.
		// Анти-туннель: за тик не больше половины дистанции до поверхности — сквозь стенку
		// не проскочить даже с колесом на максимуме (пол 1e-3 — чтобы не залипнуть намертво).
		Camera* cam = cm->GetActiveCamera();
		if (cam) {
			const glm::vec3 p_f = cam->GetPosition();
			if (g_seeded) {
				glm::dvec3 dl = glm::dvec3(p_f) * g_step_scale;
				const double dmax = std::max(0.5 * g_prev_dist, 1e-3);
				const double len = glm::length(dl);
				if (len > dmax) dl *= dmax / len;
				g_local += dl;
			}
			else {
				g_local = glm::dvec3(p_f);   // первый тик: стартовая позиция из SetView (юниты корня)
				g_seeded = true;
			}
			cam->SetPosition(glm::vec3(0.0f));
		}

		// (2) Боковые переносы: локаль вышла за ячейку → сдвиг адреса (точная целочисленная
		// арифметика, перенос любой длины за один тик) + возврат локали в [-1,1). На корне
		// (адрес пуст) координаты свободные — переносить некуда.
		if (!g_addr.empty()) {
			for (int a = 0; a < 3; ++a) {
				const double x = g_local[a];
				const long long n = (long long)std::floor((x + 1.0) * 0.5);
				if (n == 0) continue;
				if (ShiftAddrAxis(a, n)) {
					g_local[a] = x - 2.0 * (double)n;
				}
				else { FoldUpToRoot(); break; }
			}
		}

		// (2б) Потолок глубины от адреса (см. DepthCapFromAddress). Боковой перенос мог
		// СМЕНИТЬ крестовый уровень (вылет из мелкого тоннеля в крупный) — если текущая
		// глубина выше потолка, поднимаем якорь немедленно, иначе кадр рендерит фантом.
		// Каждый подъём — точная замена координат с сохранением s (σ×3 компенсирует
		// depth−1; излишек σ>=3 приберёт ренормализация ниже, тоже сохраняя s) —
		// картинка при этом НЕ меняется, контроллер плавно доведёт масштаб к новой цели.
		const int depth_cap = DepthCapFromAddress();
		while ((int)g_addr.size() > depth_cap) {
			const glm::dvec3 D = glm::dvec3(g_addr.back()) * 2.0 - 2.0;
			g_addr.pop_back();
			g_local = (g_local + D) / 3.0;
			g_sigma *= 3.0;
			g_prev_dist *= 3.0;   // дистанция прошлого тика — в юнитах кадра
		}

		// (3) АВТО-МАСШТАБ: цель σ ∝ расстоянию до поверхности (ручной сдвиг I/K поверх),
		// сглаживание в лог-пространстве с потолком скорости. Дистанция и σ — в одних юнитах
		// кадра, так что контроллер не замечает ренормализаций (обе величины ×3 синхронно).
		{
			double de = SpongeDistanceLocal(g_local);

			// Оболочка у потолка глубины (см. MIN_WALL_DIST): выталкивание на де-дистанцию
			// по градиенту DE. Коррекция ДО публикации кадра — камера просто скользит по
			// невидимой сфере вокруг стенки, дрожания нет. Вне потолка не вмешиваемся:
			// там мир мельчает вместе с камерой и коллапса не бывает.
			if ((int)g_addr.size() >= depth_cap && de < MIN_WALL_DIST) {
				const double e = 1e-5;
				const glm::dvec3 grad(
					SpongeDistanceLocal(g_local + glm::dvec3(e, 0, 0)) - SpongeDistanceLocal(g_local - glm::dvec3(e, 0, 0)),
					SpongeDistanceLocal(g_local + glm::dvec3(0, e, 0)) - SpongeDistanceLocal(g_local - glm::dvec3(0, e, 0)),
					SpongeDistanceLocal(g_local + glm::dvec3(0, 0, e)) - SpongeDistanceLocal(g_local - glm::dvec3(0, 0, e)));
				const double gl = glm::length(grad);
				if (gl > 1e-12) {
					g_local += (grad / gl) * (MIN_WALL_DIST - de);
					de = SpongeDistanceLocal(g_local);   // после проекции (у рёбер/углов не точна)
				}
			}

			const double d_loc = std::clamp(de, 1e-3, 1e9);
			const double sigma_target = AUTO_SCALE * d_loc * g_manual_mult;
			double step = SIGMA_SMOOTH * std::log(sigma_target / g_sigma);
			step = std::clamp(step, -MAX_SIGMA_STEP, MAX_SIGMA_STEP);
			g_sigma *= std::exp(step);
			g_prev_dist = d_loc;
		}

		// (4) Ренормализация масштаба переносом глубины: σ → [1,3). Подъём — всегда законен;
		// спуск требует позицию внутри ячейки (вне корня σ<1 остаётся — «микрокамера снаружи»,
		// туман ∝σ честно прячет куб). Каждый шаг — точная замена координат той же точки.
		while (g_sigma >= 3.0 && !g_addr.empty()) {
			const glm::dvec3 D = glm::dvec3(g_addr.back()) * 2.0 - 2.0;
			g_addr.pop_back();
			g_local = (g_local + D) / 3.0;
			g_sigma /= 3.0;
			g_prev_dist /= 3.0;   // дистанция — в юнитах кадра: едет вместе с ним
		}
		if (g_addr.empty() && g_sigma > MAX_OUT_SIGMA) g_sigma = MAX_OUT_SIGMA;
		while (g_sigma < 1.0 && (int)g_addr.size() < depth_cap && AbsMax3(g_local) <= 1.0) {
			glm::ivec3 idx;
			for (int a = 0; a < 3; ++a)
				idx[a] = std::clamp((int)std::floor((g_local[a] + 1.0) * 1.5), 0, 2);
			g_addr.push_back(idx);
			g_local = 3.0 * g_local - (glm::dvec3(idx) * 2.0 - 2.0);
			g_sigma *= 3.0;
			g_prev_dist *= 3.0;
		}
		if ((int)g_addr.size() >= depth_cap && g_sigma < 1.0) g_sigma = 1.0;   // упёрлись в потолок

		// (5) Публикация кадра: позиция, K, σ-непрерывные дальности, офсеты предков из
		// хвоста адреса (той же накопительной формулой, что и раньше).
		const int   d     = (int)g_addr.size();
		const int   K     = std::min(d, (int)FRACTAL_MAX_CONTEXT);
		out[0] = (float)g_local.x; out[1] = (float)g_local.y; out[2] = (float)g_local.z;
		out[3] = (float)d;
		out[4] = (float)K;
		out[5] = (float)(FOG_DIST * g_sigma);           // tFar: дальше туман насыщен, марш не нужен
		out[6] = (float)(1.0 / (FOG_DIST * g_sigma));   // 1/дальность тумана
		out[7] = 0.0f;
		glm::vec3 o(0.0f);
		float s3 = 1.0f;
		for (int j = 1; j <= (int)FRACTAL_MAX_CONTEXT; ++j) {
			if (j <= K) {
				o = (o + (glm::vec3(g_addr[d - j]) * 2.0f - 2.0f)) / 3.0f;
				s3 *= 3.0f;
			}
			float* e = out + (1 + j) * 4;
			e[0] = o.x; e[1] = o.y; e[2] = o.z; e[3] = s3;
		}

		// (6) Масштаб внесения дельты для следующего тика.
		g_step_scale = BASE_STEP * g_sigma;
	},
		[]() -> uint32_t { return (2 + FRACTAL_MAX_CONTEXT) * 4 * sizeof(float); });
}

}
