#include "common.hpp"

#include "program/setting.hpp"

extern "C" {
    /* These magic symbols are provided by the linker.  */
    extern void (*__preinit_array_start []) (void) __attribute__((weak));
    extern void (*__preinit_array_end []) (void) __attribute__((weak));
    extern void (*__init_array_start []) (void) __attribute__((weak));
    extern void (*__init_array_end []) (void) __attribute__((weak));

    /* Exported by program. */
    extern void exl_main(void*, void*);
    /* Optionally exported by program. */
    __attribute__((weak)) extern void exl_init();

    #ifdef EXL_USE_FAKEHEAP

    char __fake_heap[exl::setting::HeapSize];

    void __init_heap() {
        extern char * fake_heap_start;
        extern char * fake_heap_end;

        fake_heap_start = __fake_heap;
        fake_heap_end   = __fake_heap + exl::setting::HeapSize;
    }
    
    #endif

    void __init_array(void) {
        size_t count;
        size_t i;

        count = __preinit_array_end - __preinit_array_start;
        for (i = 0; i < count; i++)
            __preinit_array_start[i] ();

        count = __init_array_end - __init_array_start;
        for (i = 0; i < count; i++)
            __init_array_start[i] ();
    }
    
    /* Called when loaded as a module with RTLD. */
    void exl_module_init() {
        #ifdef EXL_USE_FAKEHEAP
        __init_heap();
        #endif
        exl_init();
        __init_array();
        exl_main(NULL, NULL);
    }

    /* Called when loaded as the entrypoint of the process, like RTLD. */
    void exl_entrypoint_init(void* x0, void* x1) {
        #ifdef EXL_USE_FAKEHEAP
        __init_heap();
        #endif
        exl_init();
        __init_array();
        exl_main(x0, x1);
    }

    void exl_module_fini(void) {}

}

#include <lib/util/sys/soc.hpp>
#include <lib/util/sys/mem_layout.hpp>
#include <lib/patch/patcher_impl.hpp>
#include <lib/util/version.hpp>
#include <lib/reloc/reloc.hpp>

#include <lib/log/logger_mgr.hpp>
#include <program/loggers.hpp>

#include <lib/util/sys/mem_layout.hpp>

extern "C" void exl_init() {

    /* Getting the SOC type in an application context is more effort than it's worth. */
    #ifndef EXL_AS_MODULE
/*
 * ВНИМАНИЕ. Под `#if defined(EXL_DEBUG)` здесь стоит ТОЛЬКО строка лога.
 * Вызов InitSocType() остаётся снаружи: это часть инициализации, а не логирование.
 * В romfsex (проект автора DBI) он попал внутрь условия — в релизной сборке SOC-тип
 * не определялся вовсе. При LOAD_KIND=Module этот блок и так вырезается по
 * EXL_AS_MODULE (libsetting.hpp), но для сборки AsRtld ошибка была бы настоящей.
 */
#if defined(EXL_DEBUG)
    Logging.Log(EXL_LOG_PREFIX "Inferring SOC type...");
#endif
    exl::util::impl::InitSocType();
    #endif

#if defined(EXL_DEBUG)
    Logging.Log(EXL_LOG_PREFIX "Inspecting memory layout...");
#endif
    exl::util::impl::InitMemLayout();
    virtmemSetup();
#if defined(EXL_DEBUG)
    Logging.Log(EXL_LOG_PREFIX "Initializing patcher...");
#endif
    exl::patch::impl::InitPatcherImpl();
    exl::util::impl::InitVersion();
    exl::reloc::impl::Initialize();
}