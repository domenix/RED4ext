#include "DevConsole.hpp"
#include "Platform.hpp"
#include "Utils.hpp"

DevConsole::DevConsole(const Config::DevConfig& aConfig)
    : m_isCreated(false)
    , m_stdoutStream(nullptr)
    , m_stderrStream(nullptr)
{
    if (!aConfig.hasConsole)
    {
        return;
    }

#ifdef _WIN32
    if (AllocConsole())
    {
        m_isCreated = true;

        SetConsoleTitle(L"RED4ext Console");

        // Disable the close button / context menu.
        auto console = GetConsoleWindow();
        if (console)
        {
            auto menu = GetSystemMenu(console, false);
            if (menu)
            {
                DeleteMenu(menu, SC_CLOSE, MF_BYCOMMAND);
            }
        }

        // Now redirect the output.
        auto err = freopen_s(&m_stdoutStream, "CONOUT$", "w", stdout);
        if (err != 0)
        {
            SHOW_MESSAGE_BOX_FILE_LINE(MB_ICONWARNING | MB_OK, L"Could not redirect the standard output to console.");
        }

        err = freopen_s(&m_stderrStream, "CONOUT$", "w", stderr);
        if (err != 0)
        {
            SHOW_MESSAGE_BOX_FILE_LINE(MB_ICONWARNING | MB_OK,
                                       L"Could not redirect the standard error output to console.");
        }
    }
    else
    {
        SHOW_LAST_ERROR_MESSAGE_FILE_LINE(L"Could not create the development console.");
    }
#else
    // A Windows process has no console unless it asks for one, hence AllocConsole and the
    // reopening of the standard streams. A Mac process inherits whichever streams its parent
    // gave it, so there is nothing to allocate: if the game was launched from a terminal the
    // output is already there, and if it was not, writing to the inherited descriptors is
    // still correct -- it goes to the system log.
    //
    // Nothing is created, so the destructor must not close these streams; they are not ours.
    m_isCreated = false;
    m_stdoutStream = nullptr;
    m_stderrStream = nullptr;

    m_hasInheritedConsole = true;
#endif
}

DevConsole::~DevConsole()
{
    if (m_stdoutStream)
    {
        fflush(stdout);
        fclose(m_stdoutStream);

        m_stdoutStream = nullptr;
    }

    if (m_stderrStream)
    {
        fflush(stderr);
        fclose(m_stderrStream);

        m_stderrStream = nullptr;
    }

#ifdef _WIN32
    if (m_isCreated)
    {
        FreeConsole();
    }
#endif
}

bool DevConsole::IsOutputRedirected() const
{
#ifdef _WIN32
    return m_stdoutStream || m_stderrStream;
#else
    return m_hasInheritedConsole;
#endif
}
