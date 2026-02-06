// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include <QRegularExpression>
#include <QStringBuilder>
#include <QLoggingCategory>

#include "desktopfileparser.h"
#include "constant.h"
#include "global.h"

Q_LOGGING_CATEGORY(logDesktopFileParser, "am.desktopfileparser")

namespace {
bool isInvalidLocaleString(QStringView str) noexcept
{
    // example: https://regex101.com/r/hylOay/2
    static const auto _re = [] {
        constexpr auto Language = R"((?:[a-z]+))";        // language of locale postfix. eg.(en, zh)
        constexpr auto Country = R"((?:_[A-Z]+))";        // country of locale postfix. eg.(US, CN)
        constexpr auto Encoding = R"((?:\.[0-9A-Z-]+))";  // encoding of locale postfix. eg.(UFT-8)
        constexpr auto Modifier = R"((?:@[a-zA-Z=;]+))";  // modifier of locale postfix. eg.(euro;collation=traditional)

        QRegularExpression tmp{QStringLiteral(R"(^%1%2?%3?%4?$)").arg(Language, Country, Encoding, Modifier)};
        tmp.optimize();
        return tmp;
    }();
    const auto re = _re;

    return re.matchView(str).hasMatch();
}

bool isLocaleString(QStringView key) noexcept
{
    using namespace Qt::StringLiterals;
    return (key == u"Name"_s ||
            key == u"GenericName"_s ||
            key == u"Comment"_s ||
            key == u"Keywords"_s);
}

}  // namespace

ParserError DesktopFileParser::parse(Groups &ret) noexcept
{
    std::remove_reference_t<decltype(ret)> groups;
    while (!m_stream.atEnd()) {
        auto err = addGroup(groups);
        if (err != ParserError::NoError) {
            return err;
        }

        if (groups.size() != 1) {
            continue;
        }


        if (groups.cbegin().key() != DesktopFileEntryKey) {
            qCWarning(logDesktopFileParser) << "There should be nothing preceding "
                                               "'Desktop Entry' group in the desktop entry file "
                                               "but possibly one or more comments.";
            return ParserError::InvalidFormat;
        }
    }

    if (!m_line.isEmpty()) {
        qCCritical(logDesktopFileParser) << "Something is wrong in Desktop file parser, check logic.";
        return ParserError::InternalError;
    }

    ret = std::move(groups);
    return ParserError::NoError;
}

ParserError DesktopFileParser::addGroup(Groups &ret) noexcept
{
    skip();
    if (!m_line.startsWith('[')) {
        qCDebug(logDesktopFileParser) << "Invalid desktop file format: unexpected line:" << m_line;
        return ParserError::InvalidFormat;
    }

    // Parsing group header.
    // https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#group-header

    auto groupHeader = m_line.sliced(1, m_line.size() - 2).trimmed();

    if (groupHeader.contains('[') || groupHeader.contains(']') || hasNonAsciiAndControlCharacters(groupHeader)) {
        qCDebug(logDesktopFileParser) << "group header invalid:" << m_line;
        return ParserError::InvalidFormat;
    }

    if (ret.contains(groupHeader)) {
        qCDebug(logDesktopFileParser) << "duplicated group header detected:" << groupHeader;
        return ParserError::InvalidFormat;
    }

    auto group = ret.insert(groupHeader, {});

    m_line.clear();
    while (!m_stream.atEnd() && !m_line.startsWith('[')) {
        skip();
        if (m_line.startsWith('[')) {
            break;
        }
        auto err = addEntry(group);
        if (err != ParserError::NoError) {
            return err;
        }
    }
    return ParserError::NoError;
}

ParserError DesktopFileParser::addEntry(typename Groups::iterator &group) noexcept
{
    auto line = std::move(m_line);
    m_line.clear();
    const auto lineView = QStringView{line};

    auto splitCharIndex = lineView.indexOf(u'=');
    if (splitCharIndex == -1) {
        qCDebug(logDesktopFileParser) << "invalid line in desktop file, skip it:" << line;
        return ParserError::NoError;
    }

    auto keyFullView = lineView.first(splitCharIndex).trimmed();
    auto valueView = lineView.sliced(splitCharIndex + 1).trimmed();

    // NOTE:
    // We are process "localized keys" here, for usage check:
    // https://specifications.freedesktop.org/desktop-entry-spec/desktop-entry-spec-latest.html#localized-keys
    const auto localeBegin = keyFullView.indexOf(u'[');
    const auto localeEnd = keyFullView.lastIndexOf(u']');
    if ((localeBegin == -1) != (localeEnd == -1)) {
        qCDebug(logDesktopFileParser) << "unmatched [] detected in desktop file, skip this line: " << line;
        return ParserError::NoError;
    }

    const auto keyView = (localeBegin == -1) ? keyFullView : keyFullView.first(localeBegin);
    const auto localeView =
        (localeBegin == -1) ? QStringView() : keyFullView.sliced(localeBegin + 1, localeEnd - localeBegin - 1);

    static const auto _re = [] {
        using namespace Qt::StringLiterals;
        QRegularExpression tmp{uR"([^A-Za-z0-9-])"_s};
        tmp.optimize();
        return tmp;
    }();
    // NOTE: https://stackoverflow.com/a/25583104
    const auto re = _re;
    if (re.matchView(keyView).hasMatch()) {
        qCDebug(logDesktopFileParser) << "invalid key name:" << keyView << ", skip this line:" << lineView;
        return ParserError::NoError;
    }

    const auto key = keyView.toString();
    const auto localeStr = localeView.isEmpty() ? DesktopFileDefaultKeyLocale : localeView.toString();

    if (localeStr != DesktopFileDefaultKeyLocale && !isInvalidLocaleString(localeStr)) {
        qCDebug(logDesktopFileParser).noquote() << QString("invalid LOCALE (%2) for key \"%1\"").arg(key, localeStr);
        return ParserError::NoError;
    }

    if (auto keyIt = group->find(key); keyIt != group->end()) {
        if (!isLocaleString(key)) {
            qCDebug(logDesktopFileParser) << "duplicate key:" << key << "skip.";
            return ParserError::NoError;
        }

        if (keyIt->canConvert<QStringMap>()) {
            auto localeMap = keyIt->value<QStringMap>();
            auto it = localeMap.lowerBound(localeStr);
            if (it != localeMap.end() && it.key() == localeStr) {
                qCDebug(logDesktopFileParser) << "duplicate locale key:" << key;
                return ParserError::NoError;
            }

            localeMap.insert(it, localeStr, valueView.toString());
            keyIt.value() = QVariant::fromValue(std::move(localeMap));
        }
    } else {
        if (isLocaleString(key)) {
            group->insert(key, QVariant::fromValue(QStringMap{{localeStr, valueView.toString()}}));
        } else {
            group->insert(key, QVariant::fromValue(valueView.toString()));
        }
    }

    return ParserError::NoError;
}

QString toString(const DesktopFileParser::Groups &map)
{
    QString ret;
    auto groupToString = [&ret, map](const QString &group) {
        const auto &groupEntry = map[group];
        ret.append('[' % group % "]\n");
        for (auto entryIt = groupEntry.constKeyValueBegin(); entryIt != groupEntry.constKeyValueEnd(); ++entryIt) {
            const auto &key = entryIt->first;
            const auto &value = entryIt->second;
            if (value.canConvert<QStringMap>()) {
                const auto &rawMap = value.value<QStringMap>();
                std::for_each(rawMap.constKeyValueBegin(), rawMap.constKeyValueEnd(), [key, &ret](const auto &inner) {
                    const auto &[locale, rawVal] = inner;
                    ret.append(key);
                    if (locale != DesktopFileDefaultKeyLocale) {
                        ret.append('[' % locale % ']');
                    }
                    ret.append('=' % rawVal % '\n');
                });
            } else if (value.canConvert<QStringList>()) {
                const auto &rawVal = value.value<QStringList>();
                auto str = rawVal.join(';');
                ret.append(key % '=' % str % '\n');
            } else if (value.canConvert<QString>()) {
                const auto &rawVal = value.value<QString>();
                ret.append(key % '=' % rawVal % '\n');
            } else {
                qCDebug(logDesktopFileParser) << "value type mismatch:" << value;
            }
        }
        ret.append('\n');
    };

    groupToString(DesktopFileEntryKey);
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        const auto& groupName = it.key();
        if (groupName == DesktopFileEntryKey) {
            continue;
        }
        groupToString(groupName);
    }

    return ret;
}
