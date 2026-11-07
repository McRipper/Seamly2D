// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SEAMLY_MCP_BRIDGE_H
#define SEAMLY_MCP_BRIDGE_H

#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QSet>
#ifdef Q_OS_WIN
#include <QLockFile>
#include <memory>
#endif

class MainWindow;
class QLocalSocket;

/**
 * @brief Serve private, newline-delimited GUI operations for the MCP companion.
 *
 * The bridge and all document operations run on the application's GUI thread.
 */
class McpBridge final : public QObject
{
public:
    /** @brief Attach a disabled bridge to its owning application window. */
    explicit McpBridge(MainWindow *window);

    /** @brief Stop listening and disconnect clients before the window is destroyed. */
    ~McpBridge() override;

    /**
     * @brief Validate and start an exclusive local endpoint.
     * @param socket_path Absolute Unix socket path or local Windows named pipe.
     * @return True when listening; errorString() describes a failure.
     */
    bool start(const QString &socket_path);

    /** @brief Start the configured endpoint, creating a private Unix directory if needed. */
    bool startDefault();

    /** @brief Close the listener, disconnect clients and release endpoint ownership. */
    void stop();

    /** @brief Return whether the local endpoint is accepting connections. */
    bool isListening() const
    {
        return m_server.isListening();
    }

    /** @brief Return the complete address of the active listener. */
    QString socketPath() const
    {
        return m_server.fullServerName();
    }

    /** @brief Return the number of registered MCP clients, excluding GUI probes. */
    int clientCount() const
    {
        return m_clients.size();
    }

    /** @brief Return the most recent endpoint startup error, or an empty string. */
    QString errorString() const;

private:
    MainWindow *m_window;
    QLocalServer m_server;
#ifdef Q_OS_WIN
    std::unique_ptr<QLockFile> m_pipe_lock;
#endif
    QString m_error;
    QByteArray m_session;
    int m_connections = 0;
    quint64 m_revision = 0;
    bool m_executing = false;
    QSet<QLocalSocket *> m_clients;

    /** @brief Refresh the owning window's connection status when it is available. */
    void notifyStatus();

    /** @brief Accept bounded client requests and track their connection lifetimes. */
    void acceptConnections();

    /**
     * @brief Dispatch a GUI operation after checking state and interaction constraints.
     * @param method Bridge operation requested by the companion.
     * @param arguments Operation arguments, including a state token for mutations.
     * @return Structured document, geometry, export or connection data.
     * @throws std::runtime_error If the operation is invalid or cannot be completed.
     */
    QJsonObject execute(const QString &method, const QJsonObject &arguments);

    /** @brief Return a token identifying the current document and history state. */
    QString stateToken() const;

    /** @brief Resolve the attached measurement file relative to the current pattern. */
    QString measurementsPath() const;

    /**
     * @brief Validate a private document copy in an isolated application process.
     * @param xml Candidate pattern XML.
     * @param origin Source path used to resolve relative measurement references.
     * @param allow_conversion Permit native conversion of older pattern formats.
     * @return Validated XML ready to restore into the live document.
     * @throws std::runtime_error If staging, conversion or validation fails.
     */
    QByteArray validateDocument(const QByteArray &xml, const QString &origin, bool allow_conversion = false) const;

    /**
     * @brief Restore a validated snapshot and rebuild the live pattern geometry.
     * @param xml Snapshot to restore.
     * @param active_block Draft block to reactivate after loading.
     */
    void restoreDocument(const QByteArray &xml, const QString &active_block);

    /**
     * @brief Apply a validated snapshot as a single undoable document edit.
     * @param xml Candidate snapshot to validate and commit.
     * @param label Description shown in the application's undo history.
     */
    void commitDocument(const QByteArray &xml, const QString &label);
};

#endif
