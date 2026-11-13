#include "mobaimport.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const char *what)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    // Synthetic fixture: made-up hosts, names and values.
    const QByteArray ini =
        "[Misc]\r\nRightClickAction=1\r\n"
        "[Credentials]\r\nacct=carol:SECRET-CRED-VALUE\r\n"
        "[Passwords]\r\nssh22:carol@gw.example.test=SECRET-PASSWORD-VALUE\r\n"
        "[Font]\r\nFont2=Consolas\r\nFontHeight2=12\r\n"
        "[Colors]\r\nBlack=0,0,0\r\nRed=200,0,0\r\nGreen=0,200,0\r\nYellow=200,200,0\r\n"
        "Blue=0,0,200\r\nMagenta=200,0,200\r\nCyan=0,200,200\r\nWhite=200,200,200\r\n"
        "BoldBlack=80,80,80\r\nBoldRed=255,0,0\r\nBoldGreen=0,255,0\r\nBoldYellow=255,255,0\r\n"
        "BoldBlue=0,0,255\r\nBoldMagenta=255,0,255\r\nBoldCyan=0,255,255\r\nBoldWhite=255,255,255\r\n"
        "ForegroundColour=230,230,230\r\nBackgroundColour=20,20,20\r\nCursorColour=180,180,192\r\n"
        "[Cursor]\r\nCursorType=2\r\nCursorBlinks=1\r\n"
        "[Macros]\r\n"
        "Deploy=12:0:0:ls -la __DBLDOT__ x __EQQUAL__ y|256:13:0:ENTER|258:13:0:|256:17:0:|256:67:0:|257:67:0:|257:17:0:\r\n"
        "Wait=12:0:0:echo|0:0:0:SLEEP=500\r\n"
        "Broken=garbage\r\n"
        "[MacrosHotkeys]\r\nDeploy=123\r\n"
        "[PortForwarding]\r\n"
        "0000.Db=Local;carol@gw.example.test:22;db.internal.test:5432;15432;-1;No SSH key selected;"
        "Local network adapter: 0.0.0.0;No proxy selected;0\r\n"
        "0001.Back=Remote;[acct]@gw.example.test:22;localhost:8080;9090;0;;;;0\r\n"
        "0002.Socks=Dynamic;dave@other.example.test:2222;;1080;0;C:\\keys\\id.ppk;;SOCKS5 proxy,p.test,1080;0\r\n"
        "0003.Bad=Local;carol@gw.example.test;nowhere;notaport;0\r\n"
        "[SSH_Hostkeys]\r\nssh-ed25519@22:gw.example.test=SECRET-HOSTKEY\r\n";
    const QJsonArray profiles{
        QJsonObject{{"id", "p1"}, {"protocol", "ssh"}, {"host", "GW.example.test"}, {"port", 22}, {"user", "carol"}}};
    const auto r = parseMobaExtras(ini, profiles);
    const auto s = r.settings;

    check(s["terminal/font"].toString() == "Consolas", "font");
    check(s["terminal/size"].toInt() == 12, "size");
    check(s["terminal/scheme"].toString() == "CrossTerm Dark", "schema scuro");
    check(s["terminal/rightPaste"].toBool(), "right-click paste");
    check(s["terminal/cursorShape"].toString() == "bar" && s["terminal/cursorBlink"].toBool(), "cursore");
    const auto custom = s["terminal/customColors"].toObject();
    check(custom["background"].toString() == "#141414" && custom["cursor"].toString() == "#b4b4c0", "colori fg/bg/cursore");
    check(custom["palette"].toArray().size() == 16 && custom["palette"][9].toString() == "#ff0000", "tavolozza ANSI");

    // Color scheme and context menu.
    const auto light =
        parseMobaExtras("[Colors]\nForegroundColour=40,40,40\nBackgroundColour=250,250,250\n[Misc]\nRightClickAction=0\n"
                        "[Font]\nFont2=MobaFont\n",
                        {});
    check(light.settings["terminal/scheme"].toString() == "CrossTerm Light", "schema chiaro");
    check(!light.settings["terminal/rightPaste"].toBool() && light.settings.contains("terminal/rightPaste"), "menu contestuale");
    check(!light.settings.contains("terminal/font") && !light.warnings.isEmpty(), "MobaFont not imported");
    const auto solar = parseMobaExtras("[Colors]\nForegroundColour=131,148,150\nBackgroundColour=0,43,54\n", {});
    check(solar.settings["terminal/scheme"].toString() == "Solarized Dark", "schema solarized");

    // Macros.
    check(r.macros.size() == 2, "two valid macros");
    const auto steps = r.macros[0]["steps"].toArray();
    check(r.macros[0]["name"].toString() == "Deploy" && steps.size() == 3, "Deploy steps");
    check(steps[0]["text"].toString() == "ls -la : x = y" && steps[0]["key"].toInt() == 0, "escaped text");
    check(steps[1]["key"].toInt() == Qt::Key_Return && steps[1]["modifiers"].toInt() == 0, "invio");
    check(steps[2]["key"].toInt() == Qt::Key_C && steps[2]["modifiers"].toInt() == Qt::ControlModifier, "Ctrl+C");
    check(r.macros[1]["steps"].toArray().size() == 2 && r.macros[1]["steps"].toArray()[1].toObject()["sleep"].toInt() == 500,
          "SLEEP importato come pausa");

    // Tunnels.
    check(r.tunnels.size() == 3, "tre tunnel validi");
    const auto db = r.tunnels[0].toObject();
    check(db["name"].toString() == "Db" && db["type"].toString() == "local" && db["listenPort"].toInt() == 15432 &&
              db["destHost"].toString() == "db.internal.test" && db["destPort"].toInt() == 5432 &&
              db["session"].toString() == "p1" && db["autostart"].toBool(),
          "tunnel locale associato alla sessione");
    const auto back = r.tunnels[1].toObject();
    check(back["type"].toString() == "remote" && back["session"].toString() == "p1" && !back["autostart"].toBool() &&
              back["destHost"].toString() == "localhost" && back["listenPort"].toInt() == 9090,
          "remote tunnel with credential");
    const auto socks = r.tunnels[2].toObject();
    check(socks["type"].toString() == "dynamic" && socks["listenPort"].toInt() == 1080, "tunnel dinamico");
    check(r.tunnelServers.size() == 1, "one server to create");
    const auto server = r.tunnelServers[0].toObject();
    check(server["id"].toString() == socks["session"].toString() && server["host"].toString() == "other.example.test" &&
              server["port"].toInt() == 2222 && server["user"].toString() == "dave" &&
              server["identity"].toString() == "C:\\keys\\id.ppk",
          "SSH server not linked");

    // Warnings for malformed lines.
    const auto all = r.warnings.join('\n');
    check(all.contains("Broken") && all.contains("Bad") && !all.contains("Wait") && all.contains("Socks"), "warnings");

    // Real format of a macro recorded by MobaXterm (as in public .mxtmacros files): WM_CHAR for each
    // character, Enter as WM_CHAR 13 "RETURN", "SLEEPEQUAL" pause and a final empty step.
    {
        const auto real = parseMobaExtras("[Macros]\r\nReal=258:101:2228225:e|258:99:2228225:c|258:104:2228225:h|"
                                          "258:111:2228225:o|258:13:1835009:RETURN|0:0:0:SLEEPEQUAL1200|\r\n",
                                          {});
        const auto steps = real.macros.isEmpty() ? QJsonArray{} : real.macros[0].toObject()["steps"].toArray();
        QString text;
        for (const auto &st : steps)
            text += st.toObject()["text"].toString();
        check(steps.size() == 6 && text == "echo", "real macro: text");
        check(steps.size() == 6 && steps[4].toObject()["key"].toInt() == Qt::Key_Return, "real macro: Enter");
        check(steps.size() == 6 && steps[5].toObject()["sleep"].toInt() == 1200, "real macro: pause");
        check(real.warnings.isEmpty(), "real macro: no warnings");
    }

    // No secrets in the output.
    const auto dump =
        QJsonDocument(QJsonObject{{"s", r.settings}, {"m", r.macros}, {"t", r.tunnels}, {"v", r.tunnelServers}}).toJson() +
        all.toUtf8();
    check(!dump.contains("SECRET") && !dump.contains("acct"), "no secrets");

    if (failures == 0)
        std::printf("test_mobaimport: OK\n");
    return failures ? 1 : 0;
}
