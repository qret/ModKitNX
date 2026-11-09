/*
 * nxr_fsgate.hpp — ЗАТВОР ФАЙЛОВОЙ СИСТЕМЫ: когда модулю МОЖНО трогать nn::fs.
 *
 * Copyright (c) 2026, участники проекта nxredirect. GNU GPL версии 2.
 *
 * ЗАЧЕМ ЭТОТ ФАЙЛ ПОЯВИЛСЯ (проба на консоли, Well Dweller)
 * -------------------------------------------------------------------
 * Наш модуль обращался к файловой системе РАНЬШЕ игры: хук nnMain до вызова
 * оригинала монтировал SD и открывал файлы отчётов. Игра в своём nnMain следом
 * звала nn::fs::SetAllocator — задать свой распределитель памяти для nn::fs —
 * и получала Result 0x327602 (2002-6459) «дефолтным распределителем nn::fs уже
 * пользовались», после чего аварийно останавливалась.
 *
 * Разбор nnSdk подтверждает механизм:
 *   nn::fs::SetAllocator (19.3.13 @0xdc810) проверяет байт-флаг [0xb9398c]
 *   (0xdc894: ldrb w9,[x9,#0x98c]; tbnz w9,#0 -> 0xdc960), и если флаг взведён,
 *   складывает 0x327402 + 0x200 = 0x327602 и безусловно уходит в Abort.
 *   Флаг взводит ДЕФОЛТНЫЙ распределитель nn::fs (@0xdcb90), а он вызывается из
 *   nn::fs::detail::Allocate, которым пользуется в том числе nn::fs::MountSdCard
 *   (@0xed930). То есть ЛЮБОЕ наше монтирование SD раньше игры взводит флаг и
 *   убивает игру, которая после этого зовёт SetAllocator.
 *
 * ПРАВИЛО, которое вводит этот файл: модуль не трогает nn::fs, пока затвор
 * закрыт. Затвор открывается ровно в двух местах:
 *
 *   1) ПОСЛЕ возврата из оригинала nn::fs::SetAllocator (причина SetAllocator).
 *      Игра уже поставила свой распределитель, флаг взведён ею самой, и всё,
 *      что мы сделаем дальше, пройдёт через распределитель игры. Так работает
 *      Well Dweller: его модуль `main` ИМПОРТИРУЕТ этот символ (проверено
 *      разбором .dynsym дампа: _ZN2nn2fs12SetAllocatorEPFPvmEPFvS1_mE).
 *
 *      Какой из двух путей применять, решает не догадка, а ИМПОРТЫ модулей игры:
 *      nxr::HasImportAnywhere(SetAllocator) во время работы (nxr_symfind.hpp).
 *
 *   2) ПОСЛЕ возврата из оригинала ПЕРВОГО перехваченного nn::fs::OpenFile
 *      (причина GameFsUse) — запасной момент для игр, которые SetAllocator не
 *      зовут вовсе. Так устроен SiN Reloaded: ни main, ни subsdk0..3 этого
 *      символа не импортируют (проверено там же).
 *      Почему этот момент безопасен: nn::fs требует задать распределитель ДО
 *      первого обращения к nn::fs. Если игра уже открывает файл, то либо
 *      распределитель задан, либо игра будет жить на дефолтном — и в обоих
 *      случаях SetAllocator после этого она уже не вызовет (вызвала бы — упала
 *      бы сама, без нас). Мы намеренно пропускаем этот первый вызов в оригинал
 *      БЕЗ подмены: пусть флаг взведёт сама игра, а не мы.
 *
 * Пока затвор закрыт, интересные события не выбрасываются, а складываются в
 * память (EarlyLog) и выливаются в отчёт, когда файловая система станет
 * доступна. В отчёте об этом пишется честная строка.
 *
 * Здесь ТОЛЬКО чистая логика, без nn:: и без exlaunch: ровно этот файл
 * прогоняется на ПК хост-тестом hosttest/fsgate_test.cpp с негативными пробами.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nxr {

/* Почему затвор открылся. Печатается в отчёт — пользователь должен видеть, каким
   из двух путей пошло дело на его игре. */
enum class GateReason : int {
    None         = 0,  /* затвор ещё закрыт */
    SetAllocator = 1,  /* игра вернулась из nn::fs::SetAllocator */
    GameFsUse    = 2,  /* игра сама впервые обратилась к nn::fs (OpenFile) */
};

inline const char* GateReasonText(GateReason reason) {
    switch (reason) {
        case GateReason::SetAllocator:
            return "игра вернулась из nn::fs::SetAllocator (свой распределитель задан)";
        case GateReason::GameFsUse:
            return "игра сама первой обратилась к nn::fs (SetAllocator она не зовёт)";
        case GateReason::None:
        default:
            return "затвор ещё закрыт";
    }
}

/*
 * Сам затвор. Открывается ОДИН раз и навсегда; причина первого открытия
 * не перезаписывается (иначе в отчёте оказалась бы неверная история).
 * Открывают его из разных потоков игры, поэтому состояние атомарное.
 */
class FsGate {
public:
    bool IsOpen() const {
        return m_Open.load(std::memory_order_acquire);
    }

    /*
     * Открыть затвор. Возвращает true ТОЛЬКО тому вызову, который открыл его
     * первым: именно этот вызов и выполняет отложенную работу (монтирование SD,
     * заведение отчётов). Остальные получают false и ничего не повторяют.
     */
    bool Open(GateReason reason) {
        int expected = static_cast<int>(GateReason::None);
        if (!m_Reason.compare_exchange_strong(expected, static_cast<int>(reason),
                                              std::memory_order_acq_rel)) {
            return false;
        }
        m_Open.store(true, std::memory_order_release);
        return true;
    }

    GateReason Reason() const {
        return static_cast<GateReason>(m_Reason.load(std::memory_order_acquire));
    }

private:
    std::atomic<int>  m_Reason { static_cast<int>(GateReason::None) };
    std::atomic<bool> m_Open   { false };
};

/*
 * МОНТИРОВАНИЕ SD — РОВНО ОДНА ПОПЫТКА ЗА ЗАПУСК.
 *
 * Монтируют из двух мест src/main.cpp: OpenFsGate (сборка с паспортом или
 * диагностикой — сразу при открытии затвора) и EnsureSdMounted (хук OpenFile,
 * лениво). Затвор открыт ДО того, как OpenFsGate дойдёт до монтирования, и
 * другой поток игры уже может быть в хуке OpenFile.
 *
 * Что было. OpenFsGate звал монтирование без compare-exchange: просто ставил
 * InProgress и звал MountSdCard. Если между открытием затвора и этой записью
 * другой поток в EnsureSdMounted забирал NotTried->InProgress, MountSdCard
 * вызывался ДВАЖДЫ, второй вызов получал 0x7802 («имя уже занято»), состояние
 * становилось Failed — и подмена файлов выключалась до конца сессии.
 *
 * Правило. Право на попытку забирается ОДНИМ compare-exchange NotTried->
 * InProgress в обоих путях. Ленивый путь (Ensure) при чужой попытке не ждёт и
 * отвечает «не смонтировано» — так вложенный OpenFile из самой MountSdCard не
 * уходит ни в рекурсию, ни в ожидание самого себя. Путь затвора (ForGate) ждёт
 * исхода чужой попытки: ему нужен итог для отчёта и паспорта. Ждать там
 * безопасно: OpenFsGate выполняется один раз и не изнутри MountSdCard
 * (монтирование начинается только после открытия затвора).
 *
 * Чистая логика: mount() возвращает Result (0 — успех), wait() — короткая
 * пауза. Хост-тест: fsgate_test.cpp (TestSdMountOnce), поломка gate-mount-no-cas.
 */
enum class SdState : int { NotTried = 0, InProgress = 1, Mounted = 2, Failed = 3 };

class SdMount {
public:
    SdState State() const {
        return static_cast<SdState>(m_State.load(std::memory_order_acquire));
    }

    /* Ленивый путь (хук OpenFile): true — SD смонтирована. Не ждёт. */
    template <class MountFn>
    bool Ensure(MountFn mount) {
        const SdState state = State();
        if (state == SdState::Mounted) {
            return true;
        }
        if (state == SdState::Failed || state == SdState::InProgress) {
            return false;
        }
        if (!TryClaim()) {
            /* Попытку уже делает (или сделал) другой поток. */
            return State() == SdState::Mounted;
        }
        return Run(mount) == 0;
    }

    /* Путь затвора: итог единственной попытки (своей или чужой). */
    template <class MountFn, class WaitFn>
    std::uint32_t ForGate(MountFn mount, WaitFn wait) {
        if (TryClaim()) {
            return Run(mount);
        }
        SdState state = State();
        while (state == SdState::InProgress) {
            wait();
            state = State();
        }
        return state == SdState::Mounted ? 0u : m_Result.load(std::memory_order_acquire);
    }

private:
    bool TryClaim() {
        int expected = static_cast<int>(SdState::NotTried);
        return m_State.compare_exchange_strong(expected, static_cast<int>(SdState::InProgress),
                                               std::memory_order_acq_rel);
    }

    /* Звать только тому, кто забрал право (TryClaim). */
    template <class MountFn>
    std::uint32_t Run(MountFn mount) {
        const std::uint32_t result = mount();
        m_Result.store(result, std::memory_order_release);
        m_State.store(static_cast<int>(result == 0 ? SdState::Mounted : SdState::Failed),
                      std::memory_order_release);
        return result;
    }

    std::atomic<int>           m_State  { static_cast<int>(SdState::NotTried) };
    std::atomic<std::uint32_t> m_Result { 0 };
};

/* Что делать с перехваченным OpenFile. */
enum class OpenFileAction {
    PassThroughAndWait,       /* затвор закрыт, игра ЗОВЁТ SetAllocator: только в оригинал */
    PassThroughThenOpenGate,  /* затвор закрыт, игра SetAllocator не зовёт: открыть после */
    Normal,                   /* затвор открыт: обычная работа модуля */
};

/*
 * gameCallsSetAllocator — импортирует ли хоть один модуль игры
 * nn::fs::SetAllocator (проверяется в рантайме, nxr::HasImportAnywhere).
 *
 * Почему это важно. Если игра ЗОВЁТ SetAllocator, открывать затвор по первому
 * же OpenFile опасно: вызов мог вернуть ошибку, ничего не выделив (например,
 * romfs ещё не смонтирован), флаг дефолтного распределителя остался бы не
 * взведён, а наше монтирование SD взвело бы его и убило игру на её SetAllocator.
 * Поэтому у таких игр ждём именно SetAllocator — и ничего больше.
 * У игр без этого импорта ждать нечего, и запасной путь — единственный.
 */
inline OpenFileAction DecideOpenFile(const FsGate& gate, bool gameCallsSetAllocator) {
    if (gate.IsOpen()) {
        return OpenFileAction::Normal;
    }
    if (gameCallsSetAllocator) {
        return OpenFileAction::PassThroughAndWait;
    }
    return OpenFileAction::PassThroughThenOpenGate;
}

/*
 * ПОВТОР С SD ДЛЯ ВЫЗОВА, ОТКРЫВШЕГО ЗАТВОР.
 *
 * Что было. Вызов OpenFile, по которому затвор открывается запасным путём
 * (PassThroughThenOpenGate), уходил в оригинал БЕЗ подмены — и на этом всё.
 * Если этот файл есть только на SD (его нет в romfs игры), он терялся. Так
 * терялся первый файл чужого мода: UltraCam для TotK первым же вызовом
 * открывает "content:/UltraCam/maxlastbreath.ini", а TotK SetAllocator не зовёт,
 * значит именно этот вызов и открывал затвор (разбор кода TotK).
 *
 * Правило. Если вызов шёл запасным путём И оригинал файл НЕ НАШЁЛ — повторить
 * его уже обычным путём (затвор к этому моменту открыт: монтирование SD,
 * сборка пути, подмена). Нашёл — отдаём найденное, как и раньше: открытый
 * хэндл игры не закрываем и не подменяем (чужой файл поверх уже открытого
 * не кладём — это было бы новое поведение, о котором никто не просил).
 *
 * Почему только при неудаче оригинала. Порядок «сначала оригинал» здесь
 * вынужденный: флаг дефолтного распределителя должна взвести сама игра
 * (см. шапку файла). Повтор после неудачи ничего не ломает — игра в этом
 * случае и так получила бы ошибку.
 *
 * Почему не для PassThroughAndWait. У игры, которая зовёт SetAllocator,
 * затвор после такого вызова всё ещё закрыт — повторять пока нельзя.
 *
 * Хост-тест: fsgate_test.cpp (сценарий UltraCam) и поломка no-retry-gate-opener.
 */
inline bool RetryAfterGateOpen(OpenFileAction action, bool originalFailed) {
    return action == OpenFileAction::PassThroughThenOpenGate && originalFailed;
}

/*
 * Буфер ранних событий: то, что случилось, пока писать в файл было нельзя.
 * Простая структура без конструкторов и атомиков — живёт в .bss, которую
 * загрузчик обнуляет до первой нашей инструкции (тот же приём, что у пробы
 * блокировок в nxr_gfx.hpp). Доступ к ней в модуле идёт под общим замком
 * отчёта, поэтому своего замка здесь нет.
 *
 * N — сколько событий помещается, L — предел длины пути (с нулём).
 * То, что не поместилось, НЕ теряется молча: считается в m_Dropped, и отчёт
 * печатает это число.
 */
template <int N, int L>
struct EarlyLog {
    static constexpr std::uint32_t CanaryValue = 0xA5A5A5A5u;

    struct Item {
        char m_Path[L];
        int  m_Mode;
    };

    Item          m_Items[N];
    int           m_Count;
    int           m_Dropped;
    /* Контрольное слово сразу за массивом: хост-тест проверяет, что запись
       не вышла за его край (негативная проба ring-overflow). */
    std::uint32_t m_Canary;

    void Init() {
        m_Count   = 0;
        m_Dropped = 0;
        m_Canary  = CanaryValue;
    }

    void Add(const char* path, int mode) {
        if (m_Count >= N) {
            ++m_Dropped;
            return;
        }
        Item& it = m_Items[m_Count];
        std::size_t n = 0;
        if (path != nullptr) {
            while (path[n] != '\0' && n + 1 < static_cast<std::size_t>(L)) {
                it.m_Path[n] = path[n];
                ++n;
            }
        }
        it.m_Path[n] = '\0';
        it.m_Mode    = mode;
        ++m_Count;
    }

    int         Count()   const { return m_Count; }
    int         Dropped() const { return m_Dropped; }
    bool        CanaryOk() const { return m_Canary == CanaryValue; }
    const Item& At(int i) const { return m_Items[i]; }

    void Clear() {
        m_Count   = 0;
        m_Dropped = 0;
    }
};

}  // namespace nxr
