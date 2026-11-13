#pragma once
// Localization: source strings are in English; each language is a JSON {source: translation} in :/i18n.
// A missing entry falls back to English (standard QTranslator behavior).
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QFile>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QTranslator>
#include <functional>

inline QString L(const char *source)
{
    return QCoreApplication::translate("CrossTerm", source);
}

// Marks a source string for later translation (text registry).
#define N(text) text

struct Language
{
    const char *code;
    const char *name; // native name
    bool rtl;
};

// The 30 most spoken languages in the world (total speakers, Ethnologue 2025), excluding pidgins without a standard locale.
inline const QList<Language> &languages()
{
    static const QList<Language> list{
        {"en", "English", false},
        {"zh_CN", "简体中文", false},
        {"hi", "हिन्दी", false},
        {"es", "Español", false},
        {"ar", "العربية", true},
        {"fr", "Français", false},
        {"bn", "বাংলা", false},
        {"pt_BR", "Português", false},
        {"ru", "Русский", false},
        {"ur", "اردو", true},
        {"id", "Bahasa Indonesia", false},
        {"de", "Deutsch", false},
        {"ja", "日本語", false},
        {"mr", "मराठी", false},
        {"te", "తెలుగు", false},
        {"tr", "Türkçe", false},
        {"ta", "தமிழ்", false},
        {"vi", "Tiếng Việt", false},
        {"ko", "한국어", false},
        {"ha", "Hausa", false},
        {"sw", "Kiswahili", false},
        {"fil", "Filipino", false},
        {"pa", "ਪੰਜਾਬੀ", false},
        {"fa", "فارسی", true},
        {"jv", "Basa Jawa", false},
        {"it", "Italiano", false},
        {"gu", "ગુજરાતી", false},
        {"th", "ไทย", false},
        {"kn", "ಕನ್ನಡ", false},
        {"am", "አማርኛ", false},
    };
    return list;
}

class Catalog : public QTranslator
{
public:
    QHash<QString, QString> entries;

    QString translate(const char *, const char *source, const char * = nullptr, int = -1) const override
    {
        return entries.value(QString::fromUtf8(source));
    }
    bool isEmpty() const override { return entries.isEmpty(); }
};

inline QString systemLanguage()
{
    const auto name = QLocale::system().name(); // e.g. it_IT, zh_CN, pt_BR
    for (const auto &l : languages())
    {
        if (name == l.code)
            return l.code;
    }
    for (const auto &l : languages())
    {
        if (name.section('_', 0, 0) == QString(l.code).section('_', 0, 0))
            return l.code;
    }
    return "en";
}

inline QString currentLanguage()
{
    const auto saved = QSettings().value("language").toString();
    return saved.isEmpty() ? systemLanguage() : saved;
}

inline QHash<QString, QString> loadCatalog(const QString &code)
{
    QHash<QString, QString> result;
    QFile file(":/i18n/" + code + ".json");
    if (!file.open(QIODevice::ReadOnly))
        return result;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    for (auto it = object.begin(); it != object.end(); ++it)
        result.insert(it.key(), it.value().toString());
    return result;
}

// Installs the language: Qt sends LanguageChange to every widget, which retranslate without a restart.
inline void applyLanguage(const QString &code)
{
    static Catalog *catalog = nullptr;
    if (catalog)
        QCoreApplication::removeTranslator(catalog);
    delete catalog;
    catalog = new Catalog;
    catalog->entries = loadCatalog(code);
    bool rtl = false;
    for (const auto &l : languages())
    {
        if (code == l.code)
            rtl = l.rtl;
    }
    QLocale::setDefault(QLocale(code));
    qApp->setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    QCoreApplication::installTranslator(catalog);
}

// Text registry for persistent widgets, reapplied on every language change.
class Texts
{
    QList<std::function<void()>> items;

public:
    template <class T, class Setter> T *bind(T *object, Setter setter, const char *source)
    {
        QPointer<T> guard(object);
        auto apply = [guard, setter, source]
        {
            if (guard)
                ((*guard).*setter)(L(source));
        };
        apply();
        items << apply;
        return object;
    }
    template <class T> T *text(T *object, const char *source) { return bind(object, &T::setText, source); }
    template <class T> T *tip(T *object, const char *source) { return bind(object, &T::setToolTip, source); }
    void apply() const
    {
        for (const auto &item : items)
            item();
    }
};
