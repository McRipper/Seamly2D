// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../app/seamly2d_mcp/mcp_server.h"
#include "../../app/mcp_local.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace
{
/** @brief Choose a private test endpoint with a short, unique Windows pipe name. */
QString testSocketPath(const QString &directory)
{
#ifdef Q_OS_WIN
    return QStringLiteral("\\\\.\\pipe\\seamly2d-mcp-test-") +
           QString::fromLatin1(
               QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
#else
    return QDir(directory).filePath(QStringLiteral("app.sock"));
#endif
}

/** @brief Return a syntactically valid endpoint for tests that do not call the GUI. */
QString unusedSocketPath()
{
#ifdef Q_OS_WIN
    return QStringLiteral("\\\\.\\pipe\\seamly2d-mcp-test-unused");
#else
    return QStringLiteral("/unused.sock");
#endif
}

/** @brief Build a JSON-RPC request for an MCP test client. */
QJsonObject request(const QString &method, const QJsonObject &params = {}, const QJsonValue &id = 1)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

/** @brief Build legacy initialization metadata for the requested protocol version. */
QJsonObject initializeParams(const QString &version = QStringLiteral("2025-06-18"))
{
    return {{"protocolVersion", version},
            {"capabilities", QJsonObject{}},
            {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}};
}

/** @brief Create an initialized in-process MCP session with a replaceable GUI transport. */
McpServer readyServer(McpServer::BridgeCall bridge =
                          [](const QString &, const QString &, const QJsonObject &)
                      {
                          return QJsonObject{{"state_token", QString(64, 'a')}};
                      })
{
    McpServer server(QStringLiteral("/unused.sock"), std::move(bridge));
    server.handle(request(QStringLiteral("initialize"), initializeParams()));
    server.handle(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    return server;
}

/** @brief Own a companion subprocess and exchange newline-delimited MCP messages. */
class Client
{
public:
    QProcess process;
    int next_id = 0;
    QByteArray last_line;

    /** @brief Terminate only the companion subprocess owned by this test client. */
    ~Client()
    {
        if (process.state() != QProcess::NotRunning)
        {
            process.kill();
            process.waitForFinished(3000);
        }
    }

    /** @brief Start the configured companion process for the selected local endpoint. */
    bool start(const QString &path)
    {
        QString program = qEnvironmentVariable("SEAMLY_MCP_TEST_SERVER");
        if (program.isEmpty())
        {
            program = QDir(QCoreApplication::applicationDirPath())
                          .absoluteFilePath(QStringLiteral("../../../app/seamly2d_mcp/bin/seamly2d-mcp"));
#ifdef Q_OS_WIN
            program += QStringLiteral(".exe");
#endif
        }
        process.start(program, {QStringLiteral("--socket"), path});
        return process.waitForStarted(3000);
    }

    /** @brief Write a complete JSON-RPC message and flush the process input channel. */
    void send(const QJsonObject &message)
    {
        process.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n');
        process.waitForBytesWritten(3000);
    }

    /** @brief Read one bounded-time JSON reply, returning diagnostics on failure. */
    QJsonObject receive()
    {
        QElapsedTimer timer;
        timer.start();
        while (!process.canReadLine())
        {
            if (timer.elapsed() > 5000 || !process.waitForReadyRead(5000 - static_cast<int>(timer.elapsed())))
            {
                return {{"test_error", QString::fromUtf8(process.readAllStandardError()) + process.errorString()}};
            }
        }
        const auto line = process.readLine();
        last_line = line;
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(line, &error);
        if (!document.isObject() || error.error != QJsonParseError::NoError)
        {
            return {{"test_error", QString::fromUtf8(line)}};
        }
        return document.object();
    }

    /** @brief Send a request with a fresh ID and return its response. */
    QJsonObject exchange(const QString &method, const QJsonObject &params = {})
    {
        send(request(method, params, ++next_id));
        return receive();
    }

    /** @brief Complete the legacy MCP initialization handshake. */
    bool initialize()
    {
        const auto reply = exchange(QStringLiteral("initialize"), initializeParams());
        send({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        return reply.contains("result");
    }

    /** @brief Invoke a tool and unwrap its MCP result for assertions. */
    QJsonObject call(const QString &name, const QJsonObject &arguments = {})
    {
        const auto reply = exchange(QStringLiteral("tools/call"), {{"name", name}, {"arguments", arguments}});
        if (!reply.contains("result"))
        {
            return reply;
        }
        return reply.value("result").toObject();
    }
};

/** @brief Own an isolated GUI, companion and private temporary pattern workspace. */
class ApplicationSession
{
public:
    QTemporaryDir directory{QDir::tempPath() + QStringLiteral("/smcp-XXXXXX")};
    Client application, client;
    QString pattern_path, error;

    /** @brief Launch the GUI with private files, settings, logs and an explicit endpoint. */
    bool launch(const QString &app, const QByteArray &xml, const QString &socket)
    {
        if (!directory.isValid())
        {
            return fail("Could not create test workspace.");
        }
        pattern_path = directory.filePath("test.sm2d");
        QFile pattern(pattern_path);
        if (!pattern.open(QIODevice::WriteOnly) || pattern.write(xml) != xml.size())
        {
            return fail("Could not write test pattern.");
        }
        pattern.close();
        if (!QDir(directory.path()).mkpath("state/settings/Seamly2DTeam"))
        {
            return fail("Could not create test settings.");
        }
        QFile settings(directory.filePath("state/settings/Seamly2DTeam/qt6_seamly2d.ini"));
        if (!settings.open(QIODevice::WriteOnly))
        {
            return fail("Could not write test settings.");
        }
        settings.write("[configuration]\nshowWelcome=true\nlocale=en_US\nautosave=false\n");
        settings.close();
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("SEAMLY_DATA_DIR", directory.filePath("state"));
        environment.insert("SEAMLY_MCP_SOCKET", socket);
        environment.insert("QT_QPA_PLATFORM", "offscreen");
        environment.insert("QT_LOGGING_RULES", "*.warning=false");
#ifdef Q_OS_MAC
        // Supply offscreen only to the GUI; exercise the companion's deployed frameworks.
        if (!environment.contains("DYLD_FRAMEWORK_PATH"))
        {
            environment.insert("DYLD_FRAMEWORK_PATH", QLibraryInfo::path(QLibraryInfo::LibrariesPath));
        }
        if (!environment.contains("QT_QPA_PLATFORM_PLUGIN_PATH"))
        {
            environment.insert("QT_QPA_PLATFORM_PLUGIN_PATH",
                               QLibraryInfo::path(QLibraryInfo::PluginsPath) + "/platforms");
        }
#endif
        application.process.setProcessEnvironment(environment);
        application.process.start(app, {pattern_path});
        if (!application.process.waitForStarted(3000))
        {
            return fail(application.process.errorString());
        }
        return true;
    }

    /** @brief Launch an isolated GUI, wait for its listener and initialize its MCP client. */
    bool start(const QString &app, const QByteArray &xml, const QString &endpoint = {})
    {
        const auto socket = endpoint.isEmpty() ? testSocketPath(directory.path()) : endpoint;
        if (!launch(app, xml, socket))
        {
            return false;
        }
        QElapsedTimer timer;
        timer.start();
        bool listening = false;
        while (application.process.state() != QProcess::NotRunning && timer.elapsed() < 15000)
        {
            QLocalSocket probe;
            probe.connectToServer(socket);
            if (probe.waitForConnected(50))
            {
                listening = true;
                break;
            }
            QTest::qWait(20);
        }
        if (!listening)
        {
            return fail(QString::fromUtf8(application.process.readAllStandardError()));
        }
        if (!client.start(socket))
        {
            return fail(client.process.errorString());
        }
        if (!client.initialize())
        {
            return fail("MCP initialization failed.");
        }
        return true;
    }

private:
    /** @brief Record a startup diagnostic and report that initialization failed. */
    bool fail(const QString &message)
    {
        error = message;
        return false;
    }
};

const QByteArray fixture = R"(<?xml version="1.0" encoding="UTF-8"?>
<pattern>
  <version>0.7.6</version><unit>cm</unit><description/><notes/><measurements/>
  <variables><variable name="#testLength" formula="10" description="MCP test"/></variables>
  <draftBlock name="MCP test">
    <calculation><point id="1" type="single" name="A" x="10" y="20" mx="5" my="10"/></calculation>
    <modeling/><pieces/><groups/><images/>
  </draftBlock>
</pattern>
)";
} // namespace

/** @brief Verify MCP protocol, transport and isolated GUI drafting workflows. */
class McpTest : public QObject
{
    Q_OBJECT
private slots:
    /** @brief Verify endpoint syntax and invalid command-line arguments. */
    void localEndpoints();
    /** @brief Verify initialization ordering, tool discovery and session lifecycle. */
    void lifecycle();
    /** @brief Verify compatibility with legacy MCP result envelopes. */
    void legacyProtocol();
    /** @brief Provide invalid arguments for schema validation tests. */
    void invalidArguments_data();
    /** @brief Reject missing, malformed, excessive and unsupported tool arguments. */
    void invalidArguments();
    /** @brief Verify JSON-RPC identifiers and notification response suppression. */
    void requestsAndNotifications();
    /** @brief Verify structured tool output and application error envelopes. */
    void toolResults();
    /** @brief Verify interactive UTF-8 replies without waiting for input EOF. */
    void interactiveStdio();
    /** @brief Verify malformed input handling and bounded stdio message sizes. */
    void framingAndLimits();
    /** @brief Verify local IPC framing, response validation and connection errors. */
    void bridgeTransport();
    /** @brief Verify client presence tracking and exclusion of GUI probes. */
    void connectionStatus();
    /** @brief Reject competing GUI listeners and verify Windows crash recovery. */
    void endpointExclusivity();
    /** @brief Exercise document, drawing, variables, history and export workflows. */
    void applicationIntegration();
    /** @brief Provide known geometry for mirror, move, rotation and dart operations. */
    void advancedOperations_data();
    /** @brief Verify advanced geometry, invalid edits and save/reopen behavior. */
    void advancedOperations();
    /** @brief Provide individual and multisize tables across supported document units. */
    void measurementFiles_data();
    /** @brief Verify protected measurement writes, recalculation and gradation. */
    void measurementFiles();
    /** @brief Provide point/curve contours in centimetres and millimetres. */
    void piecesAndSeamAllowances_data();
    /** @brief Verify piece contours, seam allowances, history and export. */
    void piecesAndSeamAllowances();
    /** @brief Verify modern request metadata, capabilities and catalog isolation. */
    void modernProtocol();
    /** @brief Verify skill discovery, resource integrity and allowed resource paths. */
    void skillsAndResources();
};

void McpTest::localEndpoints()
{
    QVERIFY(McpLocal::isValidSocketPath(McpLocal::defaultSocketPath()));
    QVERIFY(McpLocal::isValidSocketPath(unusedSocketPath()));
    QVERIFY(!McpLocal::isValidSocketPath(QString{}));
    QVERIFY(!McpLocal::isValidSocketPath(QStringLiteral("relative.sock")));
#ifdef Q_OS_WIN
    QVERIFY(!McpLocal::isValidSocketPath(QStringLiteral("\\\\remote\\pipe\\seamly2d-mcp")));
    QVERIFY(!McpLocal::isValidSocketPath(QStringLiteral("C:/tmp/app.sock")));
    QVERIFY(!McpLocal::isValidSocketPath(QStringLiteral("\\\\.\\pipe\\nested\\name")));
#endif
    Client client;
    QVERIFY(client.start(QStringLiteral("relative.sock")));
    QVERIFY(client.process.waitForFinished(3000) || client.process.state() == QProcess::NotRunning);
    QCOMPARE(client.process.exitCode(), 2);
    QVERIFY(client.process.readAllStandardOutput().isEmpty());
    QVERIFY(client.process.readAllStandardError().contains("--socket"));
}

void McpTest::lifecycle()
{
    McpServer server(QStringLiteral("/unused.sock"));
    QCOMPARE(server.handle(request("tools/list")).toObject().value("error").toObject().value("code").toInt(), -32600);
    QCOMPARE(server.handle(request("ping")).toObject().value("result").toObject(), QJsonObject{});
    auto result =
        server.handle(request("initialize", initializeParams("future"))).toObject().value("result").toObject();
    QCOMPARE(result.value("protocolVersion").toString(), QStringLiteral("2025-06-18"));
    QVERIFY(result.value("capabilities").toObject().contains("tools"));
    QCOMPARE(server.handle(request("tools/list")).toObject().value("error").toObject().value("code").toInt(), -32600);
    QVERIFY(server.handle(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).isUndefined());
    const auto tools =
        server.handle(request("tools/list")).toObject().value("result").toObject().value("tools").toArray();
    QCOMPARE(tools.size(), McpServer::tools().size());
    QVERIFY(tools.size() >= 30);
    for (const auto &tool : tools)
    {
        if (tool.toObject().value("name") == QJsonValue("create_point"))
        {
            QVERIFY(!tool.toObject().value("annotations").toObject().value("readOnlyHint").toBool());
        }
    }
    QCOMPARE(server.handle(request("initialize", initializeParams()))
                 .toObject()
                 .value("error")
                 .toObject()
                 .value("code")
                 .toInt(),
             -32600);
}

void McpTest::legacyProtocol()
{
    McpServer server(QStringLiteral("/unused.sock"),
                     [](const QString &, const QString &, const QJsonObject &)
                     {
                         return QJsonObject{{"ok", true}};
                     });
    const auto reply = server.handle(request("initialize", initializeParams("2024-11-05"))).toObject();
    QCOMPARE(reply.value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2024-11-05"));
    server.handle(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
    const auto result =
        server.handle(request("tools/call", {{"name", "get_selection"}})).toObject().value("result").toObject();
    QVERIFY(result.contains("content"));
    QVERIFY(!result.contains("structuredContent"));
}

void McpTest::invalidArguments_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QJsonValue>("arguments");
    QTest::newRow("missing") << QString("create_point") << QJsonValue(QJsonObject{});
    QTest::newRow("boolean") << QString("list_objects") << QJsonValue(QJsonObject{{"limit", true}});
    QTest::newRow("fraction") << QString("list_objects") << QJsonValue(QJsonObject{{"limit", 1.5}});
    QTest::newRow("large") << QString("list_objects") << QJsonValue(QJsonObject{{"limit", 201}});
    QTest::newRow("negative") << QString("list_objects") << QJsonValue(QJsonObject{{"offset", -1}});
    QTest::newRow("extra") << QString("get_selection") << QJsonValue(QJsonObject{{"extra", 1}});
    QTest::newRow("unknown") << QString("missing") << QJsonValue(QJsonObject{});
    QTest::newRow("short-token") << QString("undo") << QJsonValue(QJsonObject{{"state_token", "short"}});
    QTest::newRow("array") << QString("get_selection") << QJsonValue(QJsonArray{});
}

void McpTest::invalidArguments()
{
    QFETCH(QString, name);
    QFETCH(QJsonValue, arguments);
    bool called = false;
    auto server = readyServer(
        [&called](const QString &, const QString &, const QJsonObject &)
        {
            called = true;
            return QJsonObject{};
        });
    const auto reply = server.handle(request("tools/call", {{"name", name}, {"arguments", arguments}})).toObject();
    QCOMPARE(reply.value("error").toObject().value("code").toInt(), -32602);
    QVERIFY(!called);
}

void McpTest::requestsAndNotifications()
{
    auto server = readyServer();
    QCOMPARE(server.handle(QJsonArray{}).toObject().value("error").toObject().value("code").toInt(), -32600);
    QCOMPARE(server.handle(request("missing")).toObject().value("error").toObject().value("code").toInt(), -32601);
    QCOMPARE(server.handle(request("ping", {}, true)).toObject().value("error").toObject().value("code").toInt(),
             -32600);
    QVERIFY(server.handle(QJsonObject{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}}).isUndefined());
    QCOMPARE(server.handle(request("ping", {}, "text-id")).toObject().value("id").toString(),
             QStringLiteral("text-id"));
}

void McpTest::toolResults()
{
    auto server = readyServer();
    auto result =
        server.handle(request("tools/call", {{"name", "get_pattern_summary"}})).toObject().value("result").toObject();
    QCOMPARE(result.value("isError"), QJsonValue(false));
    const auto text = result.value("content").toArray().at(0).toObject().value("text").toString();
    QCOMPARE(QJsonDocument::fromJson(text.toUtf8()).object(), result.value("structuredContent").toObject());
    auto failing = readyServer(
        [](const QString &, const QString &, const QJsonObject &) -> QJsonObject
        {
            throw std::runtime_error("Document state changed");
        });
    result = failing.handle(request("tools/call", {{"name", "get_selection"}})).toObject().value("result").toObject();
    QCOMPARE(result.value("isError"), QJsonValue(true));
}

void McpTest::interactiveStdio()
{
    Client client;
    QVERIFY2(client.start(unusedSocketPath()), qPrintable(client.process.errorString()));
    // Keep stdin open: replies must not depend on EOF or fill a stdio buffer.
    QVERIFY(client.initialize());
    const auto reply = client.exchange(QStringLiteral("tools/list"));
    QCOMPARE(reply.value("result").toObject().value("tools").toArray().size(), McpServer::tools().size());
    QCOMPARE(client.exchange(QStringLiteral("ping")).value("result").toObject(), QJsonObject{});
    const auto unicode_id = QString::fromUtf8("cartamodello-\xc3\xa8-\xe6\xb8\xac");
    client.send(request("ping", {}, unicode_id));
    QCOMPARE(client.receive().value("id").toString(), unicode_id);
    QVERIFY(client.last_line.endsWith('\n'));
    QVERIFY(!client.last_line.contains('\r'));
    client.process.closeWriteChannel();
    QVERIFY(client.process.waitForFinished(3000));
    QCOMPARE(client.process.exitCode(), 0);
    QVERIFY(client.process.readAllStandardOutput().isEmpty());
    QVERIFY(client.process.readAllStandardError().isEmpty());
}

void McpTest::framingAndLimits()
{
    Client client;
    QVERIFY(client.start(unusedSocketPath()));
    // Text-mode stdin on Windows would treat this byte as EOF.
    client.process.write(QByteArray(1, '\x1a') + "\n");
    client.process.waitForBytesWritten(1000);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32700);
    client.process.write(
        "not json\n[]\nnull\ntrue\n42\n{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\"}\n");
    client.process.waitForBytesWritten(1000);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32700);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32600);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32600);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32600);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32600);
    client.send(request("ping"));
    QVERIFY(client.receive().contains("result"));
    client.process.write(QByteArray(8 * 1024 * 1024 + 1, 'x'));
    client.process.waitForBytesWritten(3000);
    QCOMPARE(client.receive().value("error").toObject().value("code").toInt(), -32700);
    QVERIFY(client.process.waitForFinished(3000) || client.process.state() == QProcess::NotRunning);
    QCOMPARE(client.process.exitCode(), 0);
}

void McpTest::bridgeTransport()
{
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/smcp-XXXXXX"));
    QVERIFY(directory.isValid());
    const QString path = testSocketPath(directory.path());
    QLocalServer listener;
    QVERIFY2(listener.listen(path), qPrintable(listener.errorString()));
    Client client;
    QVERIFY(client.start(path));
    QVERIFY(client.initialize());
    QVERIFY(listener.hasPendingConnections() || listener.waitForNewConnection(3000));
    std::unique_ptr<QLocalSocket> presence(listener.nextPendingConnection());
    QVERIFY(presence->canReadLine() || presence->waitForReadyRead(3000));
    QCOMPARE(QJsonDocument::fromJson(presence->readLine()).object().value("method"), QJsonValue("_mcp_session"));
    presence->write("{\"id\":0,\"result\":{");
    presence->flush();
    QTest::qWait(5);
    presence->write("\"session\":true}}\n");
    presence->flush();
    const QList<QByteArray> replies{"{\"id\":1,\"result\":{\"ok\":true}}\n",
                                    "{\"id\":1,\"error\":\"Document state changed\"}\n", "{\"id\":99,\"result\":{}}\n",
                                    "not-json\n"};
    for (int i = 0; i < replies.size(); ++i)
    {
        client.send(request("tools/call", {{"name", "get_selection"}}, i + 10));
        QVERIFY(listener.waitForNewConnection(3000));
        std::unique_ptr<QLocalSocket> connection(listener.nextPendingConnection());
        QVERIFY(connection->canReadLine() || connection->waitForReadyRead(3000));
        const auto forwarded = QJsonDocument::fromJson(connection->readLine()).object();
        QCOMPARE(forwarded.value("method").toString(), QStringLiteral("get_selection"));
        const auto bytes = replies.at(i);
        connection->write(bytes.left(bytes.size() / 2));
        connection->flush();
        QTest::qWait(5);
        connection->write(bytes.mid(bytes.size() / 2));
        connection->flush();
        const auto result = client.receive().value("result").toObject();
        QCOMPARE(result.value("isError"), QJsonValue(i != 0));
        if (i == 0)
        {
            QCOMPARE(result.value("structuredContent").toObject(), (QJsonObject{{"ok", true}}));
        }
    }
    listener.close();
    const auto failed = client.call(QStringLiteral("get_selection"));
    QCOMPARE(failed.value("isError"), QJsonValue(true));
    QVERIFY(failed.value("content").toArray().at(0).toObject().value("text").toString().contains("not retried"));
}

namespace
{
/** @brief Build a modern request with explicit per-request version and capability metadata. */
QJsonObject modernRequest(const QString &method, QJsonObject params = {}, QJsonObject extensions = {})
{
    params.insert("_meta",
                  QJsonObject{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"},
                              {"io.modelcontextprotocol/clientCapabilities", QJsonObject{{"extensions", extensions}}}});
    return request(method, params);
}
} // namespace

void McpTest::modernProtocol()
{
    McpServer server("/unused.sock");
    auto reply = server.handle(modernRequest("server/discover")).toObject();
    auto result = reply.value("result").toObject();
    QCOMPARE(result.value("resultType"), QJsonValue("complete"));
    QVERIFY(result.value("supportedVersions").toArray().contains("2026-07-28"));
    QVERIFY(result.value("capabilities")
                .toObject()
                .value("extensions")
                .toObject()
                .contains("io.modelcontextprotocol/skills"));
    QVERIFY(result.value("_meta").toObject().contains("io.modelcontextprotocol/serverInfo"));
    QVERIFY(result.contains("ttlMs"));
    // Modern requests work without initialize and never change legacy state.
    reply = server.handle(modernRequest("tools/list")).toObject();
    QCOMPARE(reply.value("result").toObject().value("tools").toArray().size(), McpServer::tools().size());
    QCOMPARE(server.handle(request("tools/list")).toObject().value("error").toObject().value("code").toInt(), -32600);
    auto unsupported = modernRequest("tools/list");
    auto params = unsupported.value("params").toObject();
    auto meta = params.value("_meta").toObject();
    meta.insert("io.modelcontextprotocol/protocolVersion", "future");
    params.insert("_meta", meta);
    unsupported.insert("params", params);
    auto detail = server.handle(unsupported).toObject().value("error").toObject();
    QCOMPARE(detail.value("code").toInt(), -32022);
    QVERIFY(detail.value("data").toObject().value("supported").toArray().contains("2026-07-28"));
    meta.insert("io.modelcontextprotocol/protocolVersion", "2026-07-28");
    meta.remove("io.modelcontextprotocol/clientCapabilities");
    params.insert("_meta", meta);
    unsupported.insert("params", params);
    QCOMPARE(server.handle(unsupported).toObject().value("error").toObject().value("code").toInt(), -32602);
    QCOMPARE(server.handle(modernRequest("ping")).toObject().value("error").toObject().value("code").toInt(), -32601);
}

void McpTest::skillsAndResources()
{
    McpServer server("/unused.sock");
    QCOMPARE(server.handle(modernRequest("skills/list")).toObject().value("error").toObject().value("code").toInt(),
             -32021);
    QJsonObject extensions{{"io.modelcontextprotocol/skills", QJsonObject{}}};
    auto result = server.handle(modernRequest("skills/list", {}, extensions)).toObject().value("result").toObject();
    const auto skill = result.value("skills").toArray().at(0).toObject();
    QCOMPARE(skill.value("frontmatter").toObject().value("name"), QJsonValue("seamly2d-pattern"));
    QCOMPARE(server.handle(modernRequest("skills/get", {{"uri", skill.value("uri")}}, extensions))
                 .toObject()
                 .value("result")
                 .toObject()
                 .value("skill"),
             QJsonValue(skill));
    for (const auto &file : skill.value("resources").toArray())
    {
        const auto entry = file.toObject();
        result = server.handle(modernRequest("resources/read", {{"uri", entry.value("uri")}}, extensions))
                     .toObject()
                     .value("result")
                     .toObject();
        const auto content = result.value("contents").toArray().at(0).toObject().value("text").toString().toUtf8();
        QCOMPARE(content.size(), entry.value("size").toInt());
        QCOMPARE("sha256:" + QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex()),
                 entry.value("digest").toString());
    }
    const auto discovery = server.handle(modernRequest("server/discover")).toObject().value("result").toObject();
    QVERIFY(!discovery.value("capabilities")
                 .toObject()
                 .value("extensions")
                 .toObject()
                 .contains("io.modelcontextprotocol/ui"));
    const auto resources = server.handle(modernRequest("resources/list", {}, extensions))
                               .toObject()
                               .value("result")
                               .toObject()
                               .value("resources")
                               .toArray();
    QCOMPARE(resources.size(), 2);
    for (const auto &resource : resources)
    {
        QCOMPARE(resource.toObject().value("mimeType"), QJsonValue("text/markdown"));
    }
    auto wrong =
        server.handle(modernRequest("resources/read", {{"uri", "skill://seamly2d-pattern/../private"}}, extensions))
            .toObject();
    QCOMPARE(wrong.value("error").toObject().value("code").toInt(), -32602);
}

void McpTest::endpointExclusivity()
{
    const auto app = qEnvironmentVariable("SEAMLY_MCP_TEST_APP");
    if (app.isEmpty())
    {
        QSKIP("Set SEAMLY_MCP_TEST_APP to run against the built Qt app.");
    }
    ApplicationSession first, second;
    QVERIFY2(first.start(app, fixture), qPrintable(first.error));
    const auto path = testSocketPath(first.directory.path());
    QVERIFY2(second.launch(app, fixture, path), qPrintable(second.error));
    QByteArray diagnostics;
    const auto loaded = [&]()
    {
        diagnostics += second.application.process.readAllStandardError();
        // Windows GUI builds may only send diagnostics to the application's log.
        const QDir logs(second.directory.filePath("state/logs/seamly2d"));
        for (const auto &file : logs.entryList({"*.log"}, QDir::Files))
        {
            QFile log(logs.filePath(file));
            if (log.open(QIODevice::ReadOnly))
            {
                diagnostics += log.readAll();
            }
        }
        // Pattern loading follows the bridge startup attempt. Keep warnings
        // disabled so unrelated Qt warnings cannot open modal test dialogs.
        return diagnostics.contains("Pattern file ") && diagnostics.contains(" was locked.");
    };
    QTRY_VERIFY2_WITH_TIMEOUT(loaded(), qPrintable(QString::fromUtf8(diagnostics)), 15000);
    QVERIFY2(!diagnostics.contains("Local MCP bridge listening at"), "A second app acquired an occupied endpoint.");
    QCOMPARE(first.client.call("get_pattern_summary").value("isError"), QJsonValue(false));
    second.application.process.kill();
    QVERIFY(second.application.process.waitForFinished(3000));
#ifdef Q_OS_WIN
    // A crashed app must release its pipe and its process lock automatically.
    first.application.process.kill();
    QVERIFY(first.application.process.waitForFinished(3000));
    ApplicationSession restarted;
    QVERIFY2(restarted.start(app, fixture, path), qPrintable(restarted.error));
    QCOMPARE(restarted.client.call("get_pattern_summary").value("isError"), QJsonValue(false));
#endif
}

void McpTest::connectionStatus()
{
    const auto app = qEnvironmentVariable("SEAMLY_MCP_TEST_APP");
    if (app.isEmpty())
    {
        QSKIP("Set SEAMLY_MCP_TEST_APP to run against the built Qt app.");
    }
    ApplicationSession session;
    QVERIFY2(session.start(app, fixture), qPrintable(session.error));
    const auto path = testSocketPath(session.directory.path());
    const auto clients = [&path]()
    {
        QLocalSocket socket;
        socket.connectToServer(path);
        if (!socket.waitForConnected(1000))
        {
            return -1;
        }
        socket.write("{\"id\":1,\"method\":\"_mcp_status\",\"params\":{}}\n");
        socket.flush();
        QElapsedTimer timer;
        timer.start();
        while (!socket.canReadLine())
        {
            if (timer.elapsed() >= 1000 || !socket.waitForReadyRead(1000 - static_cast<int>(timer.elapsed())))
            {
                return -1;
            }
        }
        const auto status = QJsonDocument::fromJson(socket.readLine()).object().value("result").toObject();
        return status.value("listening").toBool() ? status.value("clients").toInt(-1) : -1;
    };
    QTRY_COMPARE(clients(), 1);
    // A live, idle stdio client must outlast the ordinary bridge request timeout.
    QTest::qWait(5200);
    QCOMPARE(clients(), 1);
    Client second;
    QVERIFY(second.start(path));
    second.send(modernRequest("server/discover"));
    QVERIFY(second.receive().contains("result"));
    QTRY_COMPARE(clients(), 2);
    second.process.kill();
    QVERIFY(second.process.waitForFinished(3000));
    QTRY_COMPARE(clients(), 1);
    session.client.process.closeWriteChannel();
    QVERIFY(session.client.process.waitForFinished(3000));
    QTRY_COMPARE(clients(), 0);

    // The toolbar's own health check must never masquerade as a connected host.
    Client probe;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SEAMLY_MCP_PROBE", "1");
    probe.process.setProcessEnvironment(environment);
    QVERIFY(probe.start(path));
    QVERIFY(probe.initialize());
    QCOMPARE(probe.call("get_pattern_summary").value("isError"), QJsonValue(false));
    QCOMPARE(clients(), 0);
    probe.process.closeWriteChannel();
    QVERIFY(probe.process.waitForFinished(3000));
}

void McpTest::applicationIntegration()
{
    const QString app = qEnvironmentVariable("SEAMLY_MCP_TEST_APP");
    if (app.isEmpty())
    {
        QSKIP("Set SEAMLY_MCP_TEST_APP to run against the built Qt app.");
    }
    ApplicationSession session;
    QVERIFY2(session.start(app, fixture), qPrintable(session.error));
    auto &directory = session.directory;
    auto &client = session.client;
    QFile pattern(session.pattern_path);
    const auto data = [](const QJsonObject &reply)
    {
        return reply.value("structuredContent").toObject();
    };
    auto summary = data(client.call(QStringLiteral("get_pattern_summary")));
    QCOMPARE(summary.value("active_draft_block").toString(), QStringLiteral("MCP test"));
    QCOMPARE(summary.value("unit").toString(), QStringLiteral("cm"));
    QCOMPARE(summary.value("object_count").toInt(), 1);
    const auto original = data(client.call(QStringLiteral("list_objects"))).value("objects").toArray();
    QCOMPARE(original.at(0).toObject().value("x").toDouble(), 10.0);
    QCOMPARE(original.at(0).toObject().value("y").toDouble(), 20.0);
    QCOMPARE(data(client.call(QStringLiteral("get_selection"))).value("selection").toArray(), QJsonArray{});
    QJsonObject arguments{{"state_token", summary.value("state_token")},
                          {"name", "McpB"},
                          {"base_point_id", 1},
                          {"length", "#testLength"},
                          {"angle", "90"}};
    const QList<QJsonObject> invalid{{{"length", "undefined_name"}},
                                     {{"length", "-1"}},
                                     {{"length", "1/0"}},
                                     {{"name", "A"}},
                                     {{"base_point_id", 9999}}};
    for (const auto &change : invalid)
    {
        auto bad_arguments = arguments;
        for (auto entry = change.begin(); entry != change.end(); ++entry)
        {
            bad_arguments.insert(entry.key(), entry.value());
        }
        QCOMPARE(client.call(QStringLiteral("create_point"), bad_arguments).value("isError"), QJsonValue(true));
        QCOMPARE(data(client.call(QStringLiteral("get_pattern_summary"))).value("state_token"),
                 summary.value("state_token"));
    }
    const auto created_reply = client.call(QStringLiteral("create_point"), arguments);
    QCOMPARE(created_reply.value("isError"), QJsonValue(false));
    QCOMPARE(data(client.call("get_pattern_summary")).value("modified"), QJsonValue(true));
    const auto created = data(created_reply);
    const auto point = created.value("object").toObject();
    QCOMPARE(point.value("name").toString(), QStringLiteral("McpB"));
    QCOMPARE(point.value("x").toDouble(), 10.0);
    QCOMPARE(point.value("y").toDouble(), 10.0);
    QCOMPARE(data(client.call(QStringLiteral("get_pattern_summary"))).value("object_count").toInt(), 2);
    arguments.insert("name", "McpC");
    QCOMPARE(client.call(QStringLiteral("create_point"), arguments).value("isError"), QJsonValue(true));
    QCOMPARE(client.call(QStringLiteral("undo"), {{"state_token", summary.value("state_token")}}).value("isError"),
             QJsonValue(true));
    QCOMPARE(client.call(QStringLiteral("undo"), {{"state_token", created.value("state_token")}}).value("isError"),
             QJsonValue(false));
    const auto after = data(client.call(QStringLiteral("get_pattern_summary")));
    QCOMPARE(after.value("object_count").toInt(), 1);
    QVERIFY(after.value("state_token") != summary.value("state_token"));
    QCOMPARE(data(client.call(QStringLiteral("list_objects"))).value("objects").toArray(), original);

    auto current_token = [&]()
    {
        return data(client.call("get_pattern_summary")).value("state_token");
    };
    auto edit = [&](const QString &name, QJsonObject args = {})
    {
        args.insert("state_token", current_token());
        return client.call(name, args);
    };
    auto expect_success = [](const QJsonObject &reply)
    {
        return reply.value("isError") == QJsonValue(false);
    };
    QVERIFY(expect_success(client.call("evaluate_formula", {{"formula", "#testLength*2"}})));
    QCOMPARE(data(client.call("evaluate_formula", {{"formula", "#testLength*2"}})).value("value").toDouble(), 20.0);
    const auto variable_reply = edit("set_variable", {{"name", "#extra"}, {"formula", "#testLength/2"}});
    QVERIFY2(expect_success(variable_reply), qPrintable(QString::fromUtf8(QJsonDocument(variable_reply).toJson())));
    QCOMPARE(data(client.call("evaluate_formula", {{"formula", "#extra"}})).value("value").toDouble(), 5.0);
    const auto before_invalid = current_token();
    QCOMPARE(edit("set_variable", {{"name", "#extra"}, {"formula", "missing_variable"}}).value("isError"),
             QJsonValue(true));
    QCOMPARE(current_token(), before_invalid);
    QVERIFY(expect_success(edit("undo")));
    QCOMPARE(client.call("evaluate_formula", {{"formula", "#extra"}}).value("isError"), QJsonValue(true));
    QVERIFY(expect_success(edit("redo")));
    auto b = edit("create_point", {{"name", "B"}, {"base_point_id", 1}, {"length", "#extra"}, {"angle", "0"}});
    QVERIFY2(expect_success(b), qPrintable(QString::fromUtf8(QJsonDocument(b).toJson())));
    const int b_id = data(b).value("object").toObject().value("id").toInt();
    QVERIFY(expect_success(edit("create_line", {{"first_point_id", 1}, {"second_point_id", b_id}})));
    auto curve = edit("create_curve", {{"first_point_id", 1},
                                       {"second_point_id", b_id},
                                       {"angle1", "0"},
                                       {"angle2", "180"},
                                       {"length1", "2"},
                                       {"length2", "2"}});
    QVERIFY2(expect_success(curve), qPrintable(QString::fromUtf8(QJsonDocument(curve).toJson())));
    auto arc = edit("create_arc", {{"center_id", 1}, {"radius", "3"}, {"angle1", "0"}, {"angle2", "90"}});
    QVERIFY2(expect_success(arc), qPrintable(QString::fromUtf8(QJsonDocument(arc).toJson())));
    QVERIFY(expect_success(client.call("get_object", {{"id", b_id}})));
    QCOMPARE(edit("delete_variable", {{"name", "#extra"}}).value("isError"), QJsonValue(true));
    QCOMPARE(edit("delete_object", {{"id", 1}}).value("isError"), QJsonValue(true));
    QVERIFY(expect_success(edit("update_object", {{"id", b_id}, {"attributes", QJsonObject{{"length", "8"}}}})));
    QCOMPARE(data(client.call("get_object", {{"id", b_id}})).value("geometry").toObject().value("x").toDouble(), 18.0);
    QVERIFY(expect_success(edit("delete_variable", {{"name", "#extra"}})));
    auto generic =
        edit("create_tool",
             {{"section", "calculation"},
              {"xml", "<point type=\"endLine\" name=\"GenericPoint\" basePoint=\"1\" length=\"3\" angle=\"180\" "
                      "lineType=\"solidLine\" lineWeight=\"0.35\" lineColor=\"black\" mx=\"5\" my=\"10\"/>"}});
    QVERIFY2(expect_success(generic), qPrintable(QString::fromUtf8(QJsonDocument(generic).toJson())));
    int generic_id = 0;
    for (const auto &object : data(client.call("list_objects")).value("objects").toArray())
    {
        if (object.toObject().value("name") == QJsonValue("GenericPoint"))
        {
            generic_id = object.toObject().value("id").toInt();
        }
    }
    QVERIFY(generic_id > 0);
    QVERIFY(expect_success(edit("delete_object", {{"id", generic_id}})));
    QVERIFY(expect_success(edit("add_draft_block", {{"name", "Second"}, {"point_name", "C"}, {"x", 1}, {"y", 2}})));
    QCOMPARE(data(client.call("get_pattern_summary")).value("active_draft_block"), QJsonValue("Second"));
    QVERIFY(expect_success(edit("rename_draft_block", {{"name", "Second"}, {"new_name", "Renamed"}})));
    QVERIFY(expect_success(edit("set_active_draft_block", {{"name", "MCP test"}})));
    QVERIFY(expect_success(edit("set_selection", {{"ids", QJsonArray{b_id}}})));
    QVERIFY(!data(client.call("get_selection")).value("selection").toArray().isEmpty());
    QVERIFY(expect_success(edit("set_view", {{"fit", true}, {"scale", 1.0}})));
    const auto preview = data(client.call("get_preview"));
    QVERIFY(QByteArray::fromBase64(preview.value("svg_base64").toString().toLatin1()).contains("<svg"));
    for (const auto &format : {"svg", "png", "pdf"})
    {
        const QString out = directory.filePath(QString("drawing.") + format);
        auto exported = edit("export_pattern", {{"path", out}, {"format", format}});
        QVERIFY2(expect_success(exported), qPrintable(QString::fromUtf8(QJsonDocument(exported).toJson())));
        QVERIFY(QFileInfo(out).size() > 20);
        QCOMPARE(edit("export_pattern", {{"path", out}, {"format", format}}).value("isError"), QJsonValue(true));
    }
    auto xml = data(client.call("get_pattern_xml")).value("xml").toString();
    const auto unchanged = current_token();
    QCOMPARE(edit("apply_pattern_xml", {{"xml", "<!DOCTYPE pattern><pattern/>"}}).value("isError"), QJsonValue(true));
    QCOMPARE(current_token(), unchanged);
    xml.replace("<notes/>", "<notes>MCP batch</notes>");
    QVERIFY(expect_success(edit("apply_pattern_xml", {{"xml", xml}, {"label", "MCP notes"}})));
    QVERIFY(data(client.call("get_export_formats")).value("formats").toArray().size() > 10);
    QCOMPARE(data(client.call("list_pieces")).value("pieces").toArray().size(), 0);
    QCOMPARE(
        edit("export_layout", {{"output_directory", directory.filePath("layout")}, {"basename", "test"}, {"format", 0}})
            .value("isError"),
        QJsonValue(true));
    const int final_count = data(client.call("get_pattern_summary")).value("object_count").toInt();
    const int applied_count = data(client.call("get_history")).value("index").toInt();
    for (int i = 0; i < applied_count; ++i)
    {
        QVERIFY(expect_success(edit("undo")));
    }
    QCOMPARE(data(client.call("get_pattern_summary")).value("object_count").toInt(), 1);
    for (int i = 0; i < applied_count; ++i)
    {
        QVERIFY(expect_success(edit("redo")));
    }
    QCOMPARE(data(client.call("get_pattern_summary")).value("object_count").toInt(), final_count);
    const auto saved = directory.filePath("saved.sm2d");
    QVERIFY(expect_success(edit("save_pattern", {{"path", saved}})));
    QVERIFY(QFileInfo::exists(saved));
    QCOMPARE(data(client.call("get_pattern_summary")).value("modified"), QJsonValue(false));
    QVERIFY(expect_success(edit("close_pattern")));
    const auto reopened = edit("open_pattern", {{"path", saved}});
    QVERIFY2(expect_success(reopened), qPrintable(QString::fromUtf8(QJsonDocument(reopened).toJson())));
    QVERIFY(data(client.call("get_pattern_summary")).value("draft_blocks").toArray().contains("Renamed"));
    auto fresh = edit("new_pattern", {{"draft_block", "Fresh"}, {"unit", "mm"}});
    QVERIFY2(expect_success(fresh), qPrintable(QString::fromUtf8(QJsonDocument(fresh).toJson())));
    QCOMPARE(data(fresh).value("unit"), QJsonValue("mm"));
    QCOMPARE(data(fresh).value("object_count").toInt(), 1);
    QCOMPARE(edit("close_pattern").value("isError"), QJsonValue(true));
    QVERIFY(expect_success(edit("close_pattern", {{"discard_changes", true}})));

    const QString sample = QFINDTESTDATA("../CollectionTest/share/all_tools_pattern/alltools_pattern.sm2d");
    QVERIFY(!sample.isEmpty());
    const auto sample_reply = edit("open_pattern", {{"path", sample}});
    QVERIFY2(expect_success(sample_reply), qPrintable(QString::fromUtf8(QJsonDocument(sample_reply).toJson())));
    QVERIFY(data(client.call("list_pieces")).value("pieces").toArray().size() > 0);
    const auto layout = edit("export_layout", {{"output_directory", directory.filePath("pieces-svg")},
                                               {"basename", "shirt"},
                                               {"format", 0},
                                               {"pieces_only", true}});
    QVERIFY2(expect_success(layout), qPrintable(QString::fromUtf8(QJsonDocument(layout).toJson())));
    QVERIFY(!data(layout).value("files").toArray().isEmpty());
    const auto dxf = edit("export_layout", {{"output_directory", directory.filePath("pieces-dxf")},
                                            {"basename", "shirt"},
                                            {"format", 18},
                                            {"pieces_only", true}});
    QVERIFY2(expect_success(dxf), qPrintable(QString::fromUtf8(QJsonDocument(dxf).toJson())));
    QVERIFY(!data(dxf).value("files").toArray().isEmpty());
    const auto nested = edit("export_layout", {{"output_directory", directory.filePath("nested-svg")},
                                               {"basename", "layout"},
                                               {"format", 0},
                                               {"options", QJsonObject{{"pagew", "2000"},
                                                                       {"pageh", "2000"},
                                                                       {"pageunits", "mm"},
                                                                       {"ignoremargins", "true"},
                                                                       {"crop", "true"}}}});
    QVERIFY2(expect_success(nested), qPrintable(QString::fromUtf8(QJsonDocument(nested).toJson())));
    QVERIFY(!data(nested).value("files").toArray().isEmpty());
    QVERIFY(pattern.open(QIODevice::ReadOnly));
    QCOMPARE(pattern.readAll(), fixture);
}

void McpTest::advancedOperations_data()
{
    QTest::addColumn<QString>("xml");
    QTest::addColumn<QJsonObject>("attributes");
    QTest::addColumn<QJsonArray>("created_points");
    QTest::addColumn<QJsonArray>("edited_points");
    const auto points = [](double x1, double y1, double x2, double y2)
    {
        return QJsonArray{QJsonObject{{"id", 11}, {"x", x1}, {"y", y1}}, QJsonObject{{"id", 12}, {"x", x2}, {"y", y2}}};
    };
    const QString operation =
        "<operation id=\"10\" suffix=\"_copy\" %1>"
        "<source><item idObject=\"2\"/><item idObject=\"3\"/></source>"
        "<destination><item idObject=\"11\" mx=\"1\" my=\"1\"/><item idObject=\"12\" mx=\"1\" my=\"1\"/></destination>"
        "</operation>";
    QTest::newRow("rotate") << operation.arg("type=\"rotation\" center=\"1\" angle=\"90\"")
                            << QJsonObject{{"angle", "180"}} << points(0, -10, 10, -10) << points(-10, 0, -10, -10);
    // Also cover the explicit rotation origin when combining move and rotate.
    QTest::newRow("move") << operation.arg("type=\"moving\" center=\"1\" angle=\"0\" length=\"5\" rotationAngle=\"0\"")
                          << QJsonObject{{"rotationAngle", "90"}} << points(15, 0, 15, 10) << points(0, -15, 10, -15);
    QTest::newRow("mirror-line") << operation.arg("type=\"flippingByLine\" p1Line=\"1\" p2Line=\"4\"")
                                 << QJsonObject{{"p2Line", "2"}} << points(-10, 0, -10, 10) << points(10, 0, 10, -10);
    QTest::newRow("mirror-axis") << operation.arg("type=\"flippingByAxis\" center=\"1\" axisType=\"1\"")
                                 << QJsonObject{{"axisType", "2"}} << points(-10, 0, -10, 10) << points(10, 0, 10, -10);
    // A symmetric dart around (5,5) with legs at (4,0) and (6,0).
    // Closing it against the horizontal seam gives rational intersections;
    // changing the right seam endpoint to (10,10) gives the second pair.
    QTest::newRow("true-darts")
        << QString("<point id=\"10\" type=\"trueDarts\" baseLineP1=\"1\" baseLineP2=\"2\" "
                   "dartP1=\"6\" dartP2=\"5\" dartP3=\"7\" point1=\"11\" point2=\"12\" "
                   "name1=\"DartLeft\" name2=\"DartRight\" mx1=\"1\" my1=\"1\" mx2=\"1\" my2=\"1\"/>")
        << QJsonObject{{"baseLineP2", "3"}} << points(50.0 / 13, -10.0 / 13, 80.0 / 13, -10.0 / 13)
        << points(60.0 / 13, 40.0 / 13, 70.0 / 13, 40.0 / 13);
}

void McpTest::advancedOperations()
{
    const auto app = qEnvironmentVariable("SEAMLY_MCP_TEST_APP");
    if (app.isEmpty())
    {
        QSKIP("Set SEAMLY_MCP_TEST_APP to run against the built Qt app.");
    }
    QFETCH(QString, xml);
    QFETCH(QJsonObject, attributes);
    QFETCH(QJsonArray, created_points);
    QFETCH(QJsonArray, edited_points);
    const QByteArray document = R"(<pattern><version>0.7.6</version><unit>cm</unit>
<description/><notes/><measurements/><variables/><draftBlock name="Operations"><calculation>
<point id="1" type="single" name="A" x="0" y="0" mx="1" my="1"/>
<point id="2" type="endLine" name="B" basePoint="1" angle="0" length="10" lineType="solidLine" mx="1" my="1"/>
<point id="3" type="endLine" name="C" basePoint="2" angle="270" length="10" lineType="solidLine" mx="1" my="1"/>
<point id="4" type="endLine" name="D" basePoint="1" angle="270" length="10" lineType="solidLine" mx="1" my="1"/>
<point id="5" type="endLine" name="Apex" basePoint="1" angle="315" length="7.0710678118654755")"
                                R"( lineType="solidLine" mx="1" my="1"/>
<point id="6" type="endLine" name="Left" basePoint="1" angle="0" length="4" lineType="solidLine" mx="1" my="1"/>
<point id="7" type="endLine" name="Right" basePoint="1" angle="0" length="6" lineType="solidLine" mx="1" my="1"/>
</calculation><modeling/><pieces/><groups/><images/></draftBlock></pattern>
)";
    ApplicationSession session;
    QVERIFY2(session.start(app, document), qPrintable(session.error));
    auto &client = session.client;
    const auto data = [](const QJsonObject &reply)
    {
        return reply.value("structuredContent").toObject();
    };
    const auto edit = [&](const QString &name, QJsonObject args = {})
    {
        args.insert("state_token", data(client.call("get_pattern_summary")).value("state_token"));
        return client.call(name, args);
    };
    const auto success = [](const QJsonObject &reply)
    {
        return reply.value("isError") == QJsonValue(false);
    };
    QString diagnostic;
    const auto check_points = [&](const QJsonArray &expected)
    {
        const auto reply = client.call("list_objects");
        if (!success(reply))
        {
            diagnostic = QString::fromUtf8(QJsonDocument(reply).toJson());
            return false;
        }
        const auto objects = data(reply).value("objects").toArray();
        for (const auto &entry : expected)
        {
            const auto point = entry.toObject();
            bool found = false;
            for (const auto &object : objects)
            {
                const auto actual = object.toObject();
                if (actual.value("id") != point.value("id"))
                {
                    continue;
                }
                found = actual.value("type") == QJsonValue("point") && actual.value("x").isDouble() &&
                        actual.value("y").isDouble() &&
                        std::abs(actual.value("x").toDouble() - point.value("x").toDouble()) < 1e-7 &&
                        std::abs(actual.value("y").toDouble() - point.value("y").toDouble()) < 1e-7;
                diagnostic = "Expected " + QString::fromUtf8(QJsonDocument(point).toJson(QJsonDocument::Compact)) +
                             "; got " + QString::fromUtf8(QJsonDocument(actual).toJson(QJsonDocument::Compact));
                break;
            }
            if (!found)
            {
                return false;
            }
        }
        return true;
    };
    auto reply = edit("create_tool", {{"section", "calculation"}, {"xml", xml}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY2(check_points(created_points), qPrintable(diagnostic));
    // Generated points have their own IDs but share the operation's XML element.
    reply = client.call("get_object", {{"id", 11}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QCOMPARE(data(reply).value("element_id"), QJsonValue(10));
    QCOMPARE(data(reply).value("geometry").toObject().value("tool_id"), QJsonValue(10));

    const auto before_invalid = data(client.call("get_pattern_summary")).value("state_token");
    auto invalid = attributes;
    invalid.insert(xml.startsWith("<operation") ? "center" : "dartP2", "99999");
    if (xml.contains("flippingByLine"))
    {
        invalid = {{"p1Line", "99999"}};
    }
    QCOMPARE(edit("update_object", {{"id", 10}, {"attributes", invalid}}).value("isError"), QJsonValue(true));
    QCOMPARE(data(client.call("get_pattern_summary")).value("state_token"), before_invalid);
    QVERIFY2(check_points(created_points), qPrintable(diagnostic));
    reply = edit("update_object", {{"id", 10}, {"attributes", attributes}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));
    QVERIFY(success(edit("undo")));
    QVERIFY2(check_points(created_points), qPrintable(diagnostic));
    QVERIFY(success(edit("redo")));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));

    // A subsequent XML tool must not reuse either generated object's ID.
    reply = edit("create_arc", {{"center_id", 1}, {"radius", "2"}, {"angle1", "0"}, {"angle2", "90"}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));
    int arc_id = 0;
    for (const auto &object : data(client.call("list_objects")).value("objects").toArray())
    {
        if (object.toObject().value("type") == QJsonValue("arc"))
        {
            arc_id = object.toObject().value("id").toInt();
        }
    }
    QVERIFY(arc_id > 12);
    QVERIFY(success(edit("delete_object", {{"id", arc_id}})));
    QVERIFY(success(edit("undo")));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));
    QVERIFY(success(edit("redo")));
    QVERIFY(success(edit("undo")));
    QVERIFY(success(edit("undo")));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));
    QVERIFY(success(edit("undo")));
    QVERIFY(success(edit("undo")));
    QCOMPARE(data(client.call("get_pattern_summary")).value("object_count"), QJsonValue(7));
    QVERIFY(success(edit("redo")));
    QVERIFY2(check_points(created_points), qPrintable(diagnostic));
    QVERIFY(success(edit("redo")));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));
    const auto saved = session.directory.filePath("operations.sm2d");
    QVERIFY(success(edit("save_pattern", {{"path", saved}})));
    QVERIFY(success(edit("close_pattern")));
    QVERIFY(success(edit("open_pattern", {{"path", saved}})));
    QVERIFY2(check_points(edited_points), qPrintable(diagnostic));
    QFile original(session.pattern_path);
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), document);
}

void McpTest::measurementFiles_data()
{
    QTest::addColumn<QString>("unit");
    QTest::addColumn<bool>("multi");
    QTest::addColumn<QString>("pattern_unit");
    QTest::newRow("individual-cm") << QString("cm") << false << QString("cm");
    QTest::newRow("individual-mm") << QString("mm") << false << QString("cm");
    QTest::newRow("individual-inch") << QString("inch") << false << QString("cm");
    QTest::newRow("multisize-cm") << QString("cm") << true << QString("cm");
    QTest::newRow("multisize-mm") << QString("mm") << true << QString("cm");
    QTest::newRow("multisize-cm-mm-pattern") << QString("cm") << true << QString("mm");
    QTest::newRow("multisize-mm-mm-pattern") << QString("mm") << true << QString("mm");
}

void McpTest::measurementFiles()
{
    const auto app = qEnvironmentVariable("SEAMLY_MCP_TEST_APP");
    if (app.isEmpty())
    {
        QSKIP("Set SEAMLY_MCP_TEST_APP to run against the built Qt app.");
    }
    QFETCH(QString, unit);
    QFETCH(bool, multi);
    QFETCH(QString, pattern_unit);
    ApplicationSession session;
    auto model = fixture;
    model.replace("<unit>cm</unit>", "<unit>" + pattern_unit.toUtf8() + "</unit>");
    QVERIFY2(session.start(app, model), qPrintable(session.error));
    auto &client = session.client;
    const auto data = [](const QJsonObject &reply)
    {
        return reply.value("structuredContent").toObject();
    };
    const auto success = [](const QJsonObject &reply)
    {
        return reply.value("isError") == QJsonValue(false);
    };
    const auto current_token = [&]()
    {
        return data(client.call("get_pattern_summary")).value("state_token");
    };
    const auto edit = [&](const QString &name, QJsonObject args = {})
    {
        args.insert("state_token", current_token());
        return client.call(name, args);
    };
    const auto path = session.directory.filePath(multi ? "body.smms" : "body.smis");
    const double factor = unit == "mm" ? 10 : unit == "inch" ? 1 / 2.54 : 1;
    const auto entries = [&](double waist)
    {
        return multi
                   ? QJsonArray{QJsonObject{{"name", "@waist"},
                                            {"base", waist * factor},
                                            {"size_increase", 4 * factor},
                                            {"height_increase", 2 * factor}},
                                QJsonObject{{"name", "@half"}, {"base", waist * factor / 2}}}
                   : QJsonArray{QJsonObject{{"name", "@waist"}, {"formula", QString::number(waist * factor, 'g', 17)}},
                                QJsonObject{{"name", "@half"}, {"formula", "@waist/2"}}};
    };
    QJsonObject create{
        {"path", path}, {"type", multi ? "multisize" : "individual"}, {"unit", unit}, {"measurements", entries(80)}};
    if (multi)
    {
        create.insert("base_size", 44 * factor);
        create.insert("base_height", 176 * factor);
    }
    auto reply = edit("write_measurements_file", create);
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QCOMPARE(data(reply).value("undoable"), QJsonValue(false));
    const auto read_bytes = [&]()
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
        {
            return QByteArray{};
        }
        return file.readAll();
    };
    const auto original = read_bytes();
    QVERIFY(!original.isEmpty());
    reply = client.call("get_measurements_file", {{"path", path}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    auto file_token = data(reply).value("file_token");
    QCOMPARE(data(reply).value("measurements").toArray().size(), 2);
    QVERIFY(std::abs(data(reply).value("measurements").toArray().at(0).toObject().value("value").toDouble() -
                     80 * factor) < 1e-7);
    QCOMPARE(read_bytes(), original);
    QJsonObject update{
        {"path", path}, {"overwrite", true}, {"file_token", QString(64, 'a')}, {"measurements", entries(100)}};
    QCOMPARE(edit("write_measurements_file", update).value("isError"), QJsonValue(true));
    QCOMPARE(read_bytes(), original);
    update.insert("file_token", file_token);
    {
        const auto lock_path = QFileInfo(path).dir().filePath("." + QFileInfo(path).fileName() + ".locked");
        QLockFile lock(lock_path);
        lock.setStaleLockTime(0);
        QVERIFY(lock.tryLock());
        QCOMPARE(edit("write_measurements_file", update).value("isError"), QJsonValue(true));
        QCOMPARE(read_bytes(), original);
    }
    reply = edit("attach_measurements", {{"path", path}, {"file_token", file_token}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QCOMPARE(data(reply).value("type"), QJsonValue(multi ? "multisize" : "individual"));
    QCOMPARE(read_bytes(), original);
    reply = edit("create_point", {{"name", "Measured"}, {"base_point_id", 1}, {"length", "@waist/2"}, {"angle", "0"}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    const int point_id = data(reply).value("object").toObject().value("id").toInt();
    const auto point_x = [&]()
    {
        return data(client.call("get_object", {{"id", point_id}})).value("geometry").toObject().value("x").toDouble();
    };
    const auto expected_x = [&](double waist_cm)
    {
        return 10 + waist_cm * (pattern_unit == "mm" ? 10 : 1) / 2;
    };
    QVERIFY(std::abs(point_x() - expected_x(80)) < 1e-7);
    if (multi)
    {
        QCOMPARE(data(client.call("get_measurements_status")).value("active_size"), QJsonValue(44));
        reply = edit("set_gradation", {{"size", 46}, {"height", 182}});
        QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
        QCOMPARE(data(reply).value("active_size"), QJsonValue(46));
        QCOMPARE(data(reply).value("active_height"), QJsonValue(182));
        QVERIFY(std::abs(point_x() - expected_x(86)) < 1e-7);
        QCOMPARE(edit("set_gradation", {{"size", 45}, {"height", 182}}).value("isError"), QJsonValue(true));
        QVERIFY(success(edit("undo")));
        QVERIFY(std::abs(point_x() - expected_x(80)) < 1e-7);
        QVERIFY(success(edit("redo")));
        QVERIFY(std::abs(point_x() - expected_x(86)) < 1e-7);
    }
    else
    {
        QCOMPARE(edit("set_gradation", {{"size", 46}, {"height", 182}}).value("isError"), QJsonValue(true));
    }
    const auto history_index = data(client.call("get_history")).value("index");
    reply = edit("write_measurements_file", update);
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QCOMPARE(data(client.call("get_history")).value("index"), history_index);
    QCOMPARE(data(client.call("get_measurements_status")).value("needs_sync"), QJsonValue(false));
    QVERIFY(std::abs(point_x() - expected_x(multi ? 106 : 100)) < 1e-7);
    file_token = data(reply).value("file_token");
    auto edited = read_bytes();
    QVERIFY(edited != original);
    QCOMPARE(edit("write_measurements_file", update).value("isError"), QJsonValue(true));
    update.insert("file_token", file_token);
    update.insert("measurements", QJsonArray{});
    update.insert("remove", QJsonArray{"@waist"});
    QCOMPARE(edit("write_measurements_file", update).value("isError"), QJsonValue(true));
    QCOMPARE(read_bytes(), edited);
    if (!multi)
    {
        update.remove("remove");
        update.insert("measurements",
                      QJsonArray{QJsonObject{{"name", "@waist"}, {"formula", "undefined_measurement"}}});
        QCOMPARE(edit("write_measurements_file", update).value("isError"), QJsonValue(true));
        QCOMPARE(read_bytes(), edited);
    }
    {
        QFile external(path);
        QVERIFY(external.open(QIODevice::Append));
        QCOMPARE(external.write("\n"), 1);
        external.close();
        edited += '\n';
        QTRY_COMPARE_WITH_TIMEOUT(data(client.call("get_measurements_status")).value("needs_sync"), QJsonValue(true),
                                  3000);
        const auto stale =
            edit("create_point", {{"name", "Unsynced"}, {"base_point_id", 1}, {"length", "1"}, {"angle", "0"}});
        QVERIFY(stale.value("isError").toBool());
        QVERIFY(
            stale.value("content").toArray().at(0).toObject().value("text").toString().contains("changed externally"));
        const auto fresh_token = data(client.call("get_measurements_file", {{"path", path}})).value("file_token");
        reply = edit("attach_measurements", {{"path", path}, {"file_token", fresh_token}});
        QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
        QCOMPARE(data(reply).value("needs_sync"), QJsonValue(false));
        QCOMPARE(read_bytes(), edited);
    }
    const auto saved = session.directory.filePath("measured.sm2d");
    QVERIFY(success(edit("save_pattern", {{"path", saved}})));
    QVERIFY(success(edit("close_pattern")));
    reply = edit("open_pattern", {{"path", saved}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY(std::abs(point_x() - expected_x(multi ? 106 : 100)) < 1e-7);
    QCOMPARE(read_bytes(), edited);
    if (multi)
    {
        QFile saved_file(saved);
        QVERIFY(saved_file.open(QIODevice::ReadOnly));
        ApplicationSession reopened;
        QVERIFY(QFile::copy(path, reopened.directory.filePath(QFileInfo(path).fileName())));
        QVERIFY2(reopened.start(app, saved_file.readAll()), qPrintable(reopened.error));
        const auto status_reply = reopened.client.call("get_measurements_status");
        QVERIFY2(success(status_reply),
                 qPrintable(QString::fromUtf8(QJsonDocument(status_reply).toJson()) +
                            QString::fromUtf8(reopened.application.process.readAllStandardError())));
        const auto status = data(status_reply);
        QCOMPARE(status.value("active_size"), QJsonValue(46));
        QCOMPARE(status.value("active_height"), QJsonValue(182));
        const auto geometry = data(reopened.client.call("get_object", {{"id", point_id}})).value("geometry").toObject();
        QVERIFY(std::abs(geometry.value("x").toDouble() - expected_x(106)) < 1e-7);
    }
    QCOMPARE(edit("detach_measurements").value("isError"), QJsonValue(true));
    QVERIFY(success(edit("delete_object", {{"id", point_id}})));
    QVERIFY(success(edit("detach_measurements")));
    QCOMPARE(data(client.call("get_measurements_status")).value("attached"), QJsonValue(false));
    QVERIFY(success(edit("undo")));
    QCOMPARE(data(client.call("get_measurements_status")).value("attached"), QJsonValue(true));
}

void McpTest::piecesAndSeamAllowances_data()
{
    QTest::addColumn<QString>("unit");
    QTest::addColumn<bool>("curve");
    QTest::newRow("cm-points") << QString("cm") << false;
    QTest::newRow("mm-points") << QString("mm") << false;
    QTest::newRow("cm-curve") << QString("cm") << true;
    QTest::newRow("mm-curve") << QString("mm") << true;
}

void McpTest::piecesAndSeamAllowances()
{
    const auto app = qEnvironmentVariable("SEAMLY_MCP_TEST_APP");
    if (app.isEmpty())
    {
        QSKIP("Set SEAMLY_MCP_TEST_APP to run against the built Qt app.");
    }
    QFETCH(QString, unit);
    QFETCH(bool, curve);
    ApplicationSession session;
    QVERIFY2(session.start(app, fixture), qPrintable(session.error));
    auto &client = session.client;
    const auto data = [](const QJsonObject &reply)
    {
        return reply.value("structuredContent").toObject();
    };
    const auto success = [](const QJsonObject &reply)
    {
        return reply.value("isError") == QJsonValue(false);
    };
    const auto edit = [&](const QString &name, QJsonObject args = {})
    {
        args.insert("state_token", data(client.call("get_pattern_summary")).value("state_token"));
        return client.call(name, args);
    };
    QVERIFY(success(edit("new_pattern", {{"draft_block", "Panel"}, {"unit", unit}})));
    const auto point = [&](const QString &name, int base, const QString &length, const QString &angle)
    {
        return data(edit("create_point",
                         {{"name", name}, {"base_point_id", base}, {"length", length}, {"angle", angle}}))
            .value("object")
            .toObject()
            .value("id")
            .toInt();
    };
    const int b = point("B", 1, "10", "0"), c = point("C", b, "20", "270"), d = point("D", 1, "20", "270");
    QVERIFY(b > 1 && c > b && d > c);
    QJsonArray contour{QJsonObject{{"object_id", 1}}, QJsonObject{{"object_id", b}}};
    if (curve)
    {
        auto reply = edit("create_curve", {{"first_point_id", b},
                                           {"second_point_id", c},
                                           {"angle1", "270"},
                                           {"angle2", "90"},
                                           {"length1", "6"},
                                           {"length2", "6"}});
        QVERIFY(success(reply));
        int spline = 0;
        for (const auto &object : data(client.call("list_objects")).value("objects").toArray())
        {
            if (object.toObject().value("type") == QJsonValue("spline"))
            {
                spline = object.toObject().value("id").toInt();
            }
        }
        QVERIFY(spline > 0);
        contour.append(QJsonObject{{"object_id", spline}});
    }
    contour.append(QJsonObject{{"object_id", c}});
    contour.append(QJsonObject{{"object_id", d}});
    const int construction_count = data(client.call("get_pattern_summary")).value("object_count").toInt();
    auto reply = edit("create_piece", {{"name", "Panel"}, {"nodes", contour}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    const int piece = data(reply).value("id").toInt();
    QVERIFY(piece > 0);
    const auto area = [&](const QJsonObject &result, const QString &path)
    {
        return data(result).value(path).toObject().value("area").toDouble();
    };
    QVERIFY(std::abs(area(reply, "main_path") - 200) < 1e-5);
    QCOMPARE(data(reply).value("nodes").toArray().size(), contour.size());
    reply = edit("set_seam_allowance", {{"id", piece}, {"enabled", true}, {"width", "1"}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY(std::abs(area(reply, "seam_allowance_path") - 264) < 1e-5);
    const int node = data(reply).value("nodes").toArray().at(0).toObject().value("node_id").toInt();
    reply = edit(
        "set_seam_allowance",
        {{"id", piece},
         {"nodes", QJsonArray{QJsonObject{{"node_id", node}, {"before", "CurrentSeamAllowance*2"}, {"after", "2"}}}}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QCOMPARE(data(reply).value("nodes").toArray().at(0).toObject().value("before"),
             QJsonValue("CurrentSeamAllowance*2"));
    const auto unchanged = data(client.call("get_pattern_summary")).value("state_token");
    QCOMPARE(edit("set_seam_allowance", {{"id", piece}, {"width", "-1"}}).value("isError"), QJsonValue(true));
    QCOMPARE(edit("set_seam_allowance", {{"id", piece}, {"width", "unknown_formula"}}).value("isError"),
             QJsonValue(true));
    QCOMPARE(
        edit("update_piece",
             {{"id", piece}, {"nodes", QJsonArray{QJsonObject{{"object_id", 99999}}, QJsonObject{{"object_id", 1}}}}})
            .value("isError"),
        QJsonValue(true));
    QCOMPARE(data(client.call("get_pattern_summary")).value("state_token"), unchanged);
    const QJsonArray triangle{QJsonObject{{"object_id", 1}}, QJsonObject{{"object_id", b}},
                              QJsonObject{{"object_id", c}}};
    reply = edit("update_piece", {{"id", piece}, {"name", "Front"}, {"nodes", triangle}, {"in_layout", false}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY(std::abs(area(reply, "main_path") - 100) < 1e-5);
    QVERIFY(success(edit("undo")));
    QVERIFY(std::abs(area(client.call("get_piece", {{"id", piece}}), "main_path") - 200) < 1e-5);
    QVERIFY(success(edit("redo")));
    reply = edit("create_piece", {{"name", "Other"}, {"nodes", triangle}});
    QVERIFY(success(reply));
    const int other = data(reply).value("id").toInt();
    QVERIFY(success(edit("delete_piece", {{"id", other}})));
    reply = edit("update_piece", {{"id", piece}, {"in_layout", true}});
    QVERIFY(success(reply));
    const auto saved = session.directory.filePath("piece.sm2d");
    QVERIFY(success(edit("save_pattern", {{"path", saved}})));
    QVERIFY(success(edit("close_pattern")));
    QVERIFY(success(edit("open_pattern", {{"path", saved}})));
    reply = client.call("get_piece", {{"id", piece}});
    QVERIFY(success(reply));
    QCOMPARE(data(reply).value("attributes").toObject().value("name"), QJsonValue("Front"));
    QVERIFY(std::abs(area(reply, "main_path") - 100) < 1e-5);
    reply = edit("export_layout", {{"output_directory", session.directory.filePath("pieces")},
                                   {"basename", "panel"},
                                   {"format", 0},
                                   {"pieces_only", true}});
    QVERIFY2(success(reply), qPrintable(QString::fromUtf8(QJsonDocument(reply).toJson())));
    QVERIFY(!data(reply).value("files").toArray().isEmpty());
    QVERIFY(success(edit("delete_piece", {{"id", piece}})));
    QCOMPARE(data(client.call("get_pattern_summary")).value("piece_count"), QJsonValue(0));
    QCOMPARE(data(client.call("get_pattern_summary")).value("object_count"), QJsonValue(construction_count));
    QVERIFY(success(edit("undo")));
    QVERIFY(std::abs(area(client.call("get_piece", {{"id", piece}}), "main_path") - 100) < 1e-5);
    QVERIFY(success(edit("redo")));
}

QTEST_GUILESS_MAIN(McpTest)
#include "tst_mcp.moc"
