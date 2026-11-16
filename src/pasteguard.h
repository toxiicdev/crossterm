// Checks on text about to be pasted into a terminal: what a user should see before it runs.
#pragma once
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

struct PasteRisk
{
    enum Kind
    {
        Homograph,   // a word mixes Latin letters with Cyrillic or Greek ones that look the same
        Invisible,   // zero-width, non-breaking or direction-changing characters
        PipeToShell, // a downloaded script goes straight to a shell
        Profile,     // output appended or written to a shell startup file or authorized_keys
        UnpackRoot   // an archive extracted into the root folder
    };
    Kind kind;
    int line;       // 1-based
    QString detail; // the word (Homograph), the count (Invisible), the matched command otherwise
};

// Characters a reader cannot see or tell from an ordinary space, and the bidirectional controls that reorder text.
inline bool pasteInvisible(QChar c)
{
    const auto u = c.unicode();
    return u == 0x00A0 || u == 0x00AD || u == 0x115F || u == 0x1160 || u == 0x180E || (u >= 0x2000 && u <= 0x200F) ||
           (u >= 0x2028 && u <= 0x202F) || (u >= 0x205F && u <= 0x206F) || u == 0x3000 || u == 0x3164 || u == 0xFEFF ||
           u == 0xFFA0;
}

// Letters of Cyrillic and Greek that have a Latin twin (а е о р с у х ...): only these make a word look Latin.
inline bool pasteLookalike(QChar c)
{
    static const QString twins = QString::fromUtf8("АВЕЅІЈКМНОРСТХУаеіјорсухѕԁһӏԛԝ"
                                                   "ΑΒΕΖΗΙΚΜΝΟΡΤΥΧανορτυχικ");
    return twins.contains(c);
}

inline QList<PasteRisk> pasteRisks(const QString &text)
{
    QList<PasteRisk> risks;
    const auto lines = QString(text).replace("\r\n", "\n").replace('\r', '\n').split('\n');
    static const QRegularExpression word("\\S+");
    // curl/wget/fetch output piped to a shell or interpreter; bash <(curl ...); sh -c "$(curl ...)"; PowerShell iwr | iex.
    static const QRegularExpression pipe(
        "\\b(?:curl|wget|fetch|iwr|irm|Invoke-WebRequest|Invoke-RestMethod)\\b[^\\n|]*\\|\\s*(?:sudo\\s+(?:-\\S+\\s+)*)?"
        "(?:env\\s+)?(?:\\S*/)?(?:ba|z|da|k|c|tc|fi|a)?sh\\b|"
        "\\b(?:curl|wget|fetch|iwr|irm|Invoke-WebRequest|Invoke-RestMethod)\\b[^\\n|]*\\|\\s*(?:sudo\\s+)?(?:\\S*/)?(?:python[0-9.]*|perl|ruby|node|php|iex|Invoke-Expression)\\b|"
        "\\b(?:ba|z|k)?sh\\s+(?:-c\\s+)?[\"']?(?:<\\(|\\$\\()\\s*(?:curl|wget|fetch)\\b|"
        "\\b(?:iex|Invoke-Expression)\\b[^\\n]*\\b(?:iwr|irm|Invoke-WebRequest|Invoke-RestMethod|DownloadString)\\b",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression profile(
        "(?:>>?|\\btee\\b(?:\\s+-\\S+)*)\\s*[\"']?(?:~|\\$HOME|\\$\\{HOME\\}|/root|/home/[^/\\s]+)?/?"
        "(?:\\.bashrc|\\.bash_profile|\\.bash_login|\\.profile|\\.zshrc|\\.zprofile|\\.zshenv|\\.zlogin|\\.kshrc|\\.cshrc|"
        "\\.tcshrc|\\.config/fish/config\\.fish|\\.ssh/authorized_keys2?|\\.ssh/rc)\\b|"
        "(?:>>?|\\btee\\b(?:\\s+-\\S+)*)\\s*[\"']?/etc/(?:profile|bash\\.bashrc|zshrc|zsh/zshrc|environment|ld\\.so\\.preload|"
        "profile\\.d/\\S+|sudoers(?:\\.d/\\S+)?|crontab|cron\\.d/\\S+|rc\\.local)\\b");
    static const QRegularExpression root(
        "\\b(?:tar|bsdtar|gtar)\\b[^\\n;&|]*(?:\\s-C\\s*|\\s--directory[=\\s]\\s*)[\"']?/[\"']?(?=\\s|$|;|&|\\|)|"
        "\\bunzip\\b[^\\n;&|]*\\s-d\\s*[\"']?/[\"']?(?=\\s|$|;|&|\\|)|"
        "\\bcd\\s+/\\s*(?:&&|;)\\s*(?:sudo\\s+)?(?:tar\\s+\\S*x|unzip\\b)");
    for (int i = 0; i < lines.size(); ++i)
    {
        const auto &line = lines[i];
        int invisible = 0;
        for (const auto c : line)
            invisible += pasteInvisible(c);
        if (invisible)
            risks.append({PasteRisk::Invisible, i + 1, QString::number(invisible)});
        for (auto it = word.globalMatch(line); it.hasNext();)
        {
            const auto w = it.next().captured();
            bool latin = false, twin = false;
            for (const auto c : w)
            {
                latin = latin || (c.unicode() < 0x80 && c.isLetter());
                twin = twin || pasteLookalike(c);
            }
            if (latin && twin)
            {
                risks.append({PasteRisk::Homograph, i + 1, w});
                break;
            }
        }
        if (const auto m = pipe.match(line); m.hasMatch())
            risks.append({PasteRisk::PipeToShell, i + 1, m.captured().trimmed()});
        if (const auto m = profile.match(line); m.hasMatch())
            risks.append({PasteRisk::Profile, i + 1, m.captured().trimmed()});
        if (const auto m = root.match(line); m.hasMatch())
            risks.append({PasteRisk::UnpackRoot, i + 1, m.captured().trimmed()});
    }
    return risks;
}

// Lines a paste sends: a final line end runs the last command too, so it counts as a line of its own.
inline int pasteLineCount(const QString &text)
{
    return int(QString(text).replace("\r\n", "\n").count(QRegularExpression("[\\r\\n]"))) + 1;
}

// The text with invisible characters dropped and the space-like ones turned into ordinary spaces.
inline QString pasteCleaned(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const auto c : text)
    {
        if (!pasteInvisible(c))
            out += c;
        else if (c.unicode() == 0x00A0 || (c.unicode() >= 0x2000 && c.unicode() <= 0x200A) || c.unicode() == 0x202F ||
                 c.unicode() == 0x205F || c.unicode() == 0x3000)
            out += ' ';
    }
    return out;
}
