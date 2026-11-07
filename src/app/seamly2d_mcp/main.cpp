// SPDX-License-Identifier: GPL-3.0-or-later
#include "mcp_server.h"
#include "../mcp_local.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <cstdio>
#ifdef Q_OS_UNIX
#include <csignal>
#endif
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

/** @brief Validate the local endpoint and serve MCP over binary UTF-8 stdio. */
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("seamly2d-mcp"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.4.0"));
    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Local Seamly2D MCP stdio adapter. Requires the enabled application bridge."));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption socket_option(
        QStringLiteral("socket"),
        QStringLiteral("Local socket path or Windows named pipe used by the app's SEAMLY_MCP_SOCKET."),
        QStringLiteral("path"), qEnvironmentVariable("SEAMLY_MCP_SOCKET", McpLocal::defaultSocketPath()));
    parser.addOption(socket_option);
    parser.process(app);
    const QString socket_path = parser.value(socket_option);
    if (!parser.positionalArguments().isEmpty() || !McpLocal::isValidSocketPath(socket_path))
    {
        std::fprintf(stderr,
                     "seamly2d-mcp: --socket (or SEAMLY_MCP_SOCKET) must be %s; no positional arguments accepted.\n",
                     McpLocal::socketPathRequirement());
        return 2;
    }
#ifdef Q_OS_UNIX
    // The client may close its read end while a request is completing.
    std::signal(SIGPIPE, SIG_IGN);
#endif
#ifdef Q_OS_WIN
    // Preserve UTF-8 bytes, LF framing and Ctrl-Z inside the stdio transport.
    if (_setmode(_fileno(stdin), _O_BINARY) == -1 || _setmode(_fileno(stdout), _O_BINARY) == -1)
    {
        std::fputs("seamly2d-mcp: cannot set stdin/stdout to binary mode.\n", stderr);
        return 1;
    }
#endif
    QFile input;
    QFile output;
    if (!input.open(stdin, QIODevice::ReadOnly) || !output.open(stdout, QIODevice::WriteOnly))
    {
        std::fputs("seamly2d-mcp: cannot open stdin/stdout.\n", stderr);
        return 1;
    }
    McpServer server(socket_path);
    return serveMcp(server, input, output);
}
