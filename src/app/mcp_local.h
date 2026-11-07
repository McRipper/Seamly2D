// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SEAMLY_MCP_LOCAL_H
#define SEAMLY_MCP_LOCAL_H

#include <QCryptographicHash>
#include <QDir>
#include <QRegularExpression>

namespace McpLocal
{
/**
 * @brief Return the user's default local socket path or Windows named pipe.
 * @return A stable endpoint shared by the GUI and its companion process.
 */
inline QString defaultSocketPath()
{
#ifdef Q_OS_WIN
    // Keep the default stable across GUI/companion processes and separate users.
    const auto user = QDir::cleanPath(QDir::homePath()).toCaseFolded().toUtf8();
    return QStringLiteral("\\\\.\\pipe\\seamly2d-mcp-") +
           QString::fromLatin1(QCryptographicHash::hash(user, QCryptographicHash::Sha256).toHex().left(24));
#else
    return QDir::homePath() + QStringLiteral("/.seamly2d/mcp/app.sock");
#endif
}

/**
 * @brief Check the platform-specific syntax of a local MCP endpoint.
 * @param path Absolute Unix socket path or local Windows named pipe.
 * @return True when the syntax is valid; ownership is checked by the bridge.
 */
inline bool isValidSocketPath(const QString &path)
{
#ifdef Q_OS_WIN
    const QString prefix = QStringLiteral("\\\\.\\pipe\\");
    static const QRegularExpression name(QStringLiteral("\\A[A-Za-z0-9][A-Za-z0-9._-]{0,199}\\z"));
    return path.startsWith(prefix) && name.match(path.mid(prefix.size())).hasMatch();
#else
    return !path.isEmpty() && QDir::isAbsolutePath(path);
#endif
}

/**
 * @brief Describe the endpoint syntax required on the current platform.
 * @return Static text suitable for command-line and GUI validation errors.
 */
inline const char *socketPathRequirement()
{
#ifdef Q_OS_WIN
    return "a local named pipe such as \\\\.\\pipe\\seamly2d-mcp (letters, digits, dots, underscores and hyphens only)";
#else
    return "an absolute local socket path";
#endif
}
} // namespace McpLocal

#endif
