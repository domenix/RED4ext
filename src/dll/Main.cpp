#include "App.hpp"
#include "Image.hpp"
#include "Platform.hpp"
#include "Utils.hpp"

namespace
{
/**
 * Shared entry logic. Both platforms run this once the runtime has been mapped into the game,
 * and its counterpart once before it goes away; only the mechanism that gets us here differs.
 */
void Startup()
{
    try
    {
        const auto image = Image::Get();
        if (!image->IsCyberpunk())
        {
            return;
        }

        App::Construct();
    }
    catch (const std::exception& e)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"An exception occured while loading RED4ext.\n\n{}",
                                            Utils::Widen(e.what()));
    }
    catch (...)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"An unknown exception occured while loading RED4ext.");
    }
}

void Teardown()
{
    try
    {
        const auto image = Image::Get();
        if (!image->IsCyberpunk())
        {
            return;
        }

        App::Destruct();
    }
    catch (const std::exception& e)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"An exception occured while unloading RED4ext.\n\n{}",
                                            Utils::Widen(e.what()));
    }
    catch (...)
    {
        SHOW_MESSAGE_BOX_AND_EXIT_FILE_LINE(L"An unknown exception occured while unloading RED4ext.");
    }
}
} // namespace

#ifdef _WIN32

BOOL APIENTRY DllMain(HMODULE aModule, DWORD aReason, LPVOID aReserved)
{
    RED4EXT_UNUSED_PARAMETER(aReserved);

    switch (aReason)
    {
    case DLL_PROCESS_ATTACH:
    {
        DisableThreadLibraryCalls(aModule);
        Startup();

        break;
    }
    case DLL_PROCESS_DETACH:
    {
        if (aReserved != nullptr)
        {
            // DLL is being unloaded due to process termination, skip cleanup.
            // https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain
            break;
        }

        Teardown();
        break;
    }
    }

    return TRUE;
}

#else

// There is no winmm proxy on macOS, and no DllMain. RED4ext is brought in with
// DYLD_INSERT_LIBRARIES, so dyld runs the constructor below before the game's main -- the same
// point in startup DLL_PROCESS_ATTACH gave us on Windows: the image is mapped and its code can
// be patched, but none of it has run yet.
//
// The work happens inline rather than on a worker thread deliberately. Hooks must be in place
// before the game starts executing, and nothing on this path loads another library (plugins
// are loaded much later, from game-state callbacks), so there is no dyld lock to deadlock on.

__attribute__((constructor)) static void RED4extLoad()
{
    Startup();
}

__attribute__((destructor)) static void RED4extUnload()
{
    Teardown();
}

#endif
