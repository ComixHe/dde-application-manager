// SPDX-FileCopyrightText: 2023 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: LGPL-3.0-or-later

#include "dbus/applicationservice.h"
#include "applicationmanager1service.h"
#include "dbus/instanceadaptor.h"
#include "pwd.h"
#include <QUuid>
#include <QStringList>
#include <QList>
#include <QUrl>
#include <QRegularExpression>
#include <QProcess>
#include <algorithm>
#include <new>

ApplicationService::ApplicationService(DesktopFile source, ApplicationManager1Service *parent)
    : QObject(parent)
    , m_desktopSource(std::move(source))
{
}

ApplicationService::~ApplicationService()
{
    for (auto &instance : m_Instances.values()) {
        instance->m_Application = QDBusObjectPath{"/"};
        auto *ptr = instance.get();
        instance.reset(nullptr);
        ptr->setParent(qApp);  // detach all instances to qApp
    }
}

QSharedPointer<ApplicationService> ApplicationService::createApplicationService(DesktopFile source,
                                                                                ApplicationManager1Service *parent) noexcept
{
    QString objectPath;
    QTextStream sourceStream;
    QFile sourceFile;
    QString tempSource;
    QSharedPointer<ApplicationService> app{nullptr};

    if (source.persistence()) {
        objectPath = QString{DDEApplicationManager1ObjectPath} + "/" + escapeToObjectPath(source.desktopId());
        sourceFile.setFileName(source.fileSource());
        if (!sourceFile.open(QFile::ExistingOnly | QFile::ReadOnly | QFile::Text)) {
            qCritical() << "desktop file can't open:" << source.fileSource() << sourceFile.errorString();
            return nullptr;
        }

        app.reset(new (std::nothrow) ApplicationService{std::move(source), parent});
        sourceStream.setDevice(&sourceFile);
    } else {
        tempSource = source.fileSource();
        objectPath = QString{DDEApplicationManager1ObjectPath} + "/" + QUuid::createUuid().toString(QUuid::Id128);
        app.reset(new (std::nothrow) ApplicationService{std::move(source), parent});
        sourceStream.setString(&tempSource, QTextStream::ReadOnly | QTextStream::Text);
    }
    std::unique_ptr<DesktopEntry> entry{std::make_unique<DesktopEntry>()};
    auto error = entry->parse(sourceStream);

    if (error != DesktopErrorCode::NoError) {
        if (error != DesktopErrorCode::EntryKeyInvalid) {
            return nullptr;
        }
    }

    if (auto val = entry->value(DesktopFileEntryKey, "Hidden"); val.has_value()) {
        bool ok{false};
        if (auto hidden = val.value().toBoolean(ok); ok and hidden) {
            return nullptr;
        }
    }

    app->m_entry.reset(entry.release());
    app->m_applicationPath = QDBusObjectPath{std::move(objectPath)};

    // TODO: icon lookup
    new (std::nothrow) APPObjectManagerAdaptor{app.data()};
    return app;
}

QString ApplicationService::GetActionName(const QString &identifier, const QStringList &env) const
{
    const auto &supportedActions = actions();
    if (supportedActions.isEmpty()) {
        return {};
    }
    if (auto index = supportedActions.indexOf(identifier); index == -1) {
        qWarning() << "can't find " << identifier << " in supported actions List.";
        return {};
    }

    const auto &actionHeader = QString{"%1%2"}.arg(DesktopFileActionKey, identifier);
    const auto &actionName = m_entry->value(actionHeader, "Name");
    if (!actionName) {
        return {};
    }

    QString locale{""};
    bool ok;
    if (!env.isEmpty()) {
        QString lcStr;
        for (const auto &lc : env) {
            if (lc.startsWith("LANG")) {
                locale = lc.split('=').last();
            }

            if (lc.startsWith("LC_ALL")) {
                locale = lc.split('=').last();
                break;
            }
        }
    }

    QLocale lc = locale.isEmpty() ? getUserLocale() : QLocale{locale};

    const auto &name = actionName->toLocaleString(lc, ok);
    if (!ok) {
        qWarning() << "convert to locale string failed, dest locale:" << lc;
        return {};
    }
    return name;
}

QString ApplicationService::GetDisplayName(const QStringList &env) const
{
    const auto &displayName = m_entry->value(DesktopFileEntryKey, "Name");
    if (!displayName) {
        return {};
    }

    QString locale{""};
    bool ok;
    if (!env.isEmpty()) {
        QString lcStr;
        for (const auto &lc : env) {
            if (lc.startsWith("LANG")) {
                locale = lc.split('=').last();
            }

            if (lc.startsWith("LC_ALL")) {
                locale = lc.split('=').last();
                break;
            }
        }
    }

    QLocale lc = locale.isEmpty() ? getUserLocale() : QLocale{locale};

    const auto &name = displayName->toLocaleString(lc, ok);
    if (!ok) {
        qWarning() << "convert to locale string failed, dest locale:" << lc;
        return {};
    }
    return name;
}

QString ApplicationService::GetIconName(const QString &action) const
{
    std::optional<DesktopEntry::Value> iconName{std::nullopt};

    if (action.isEmpty()) {
        iconName = m_entry->value(DesktopFileEntryKey, "Icon");

    } else {
        const auto &supportedActions = actions();

        if (auto index = supportedActions.indexOf(action); index == -1) {
            qWarning() << "can't find " << action << " in supported actions List.";
            return {};
        }

        const auto &actionHeader = QString{"%1%2"}.arg(DesktopFileActionKey, action);
        iconName = m_entry->value(actionHeader, "Icon");
    }

    if (!iconName) {
        return {};
    }

    bool ok{false};
    const auto &name = iconName->toIconString(ok);
    return ok ? name : "";
}

QDBusObjectPath ApplicationService::Launch(const QString &action, const QStringList &fields, const QVariantMap &options)
{
    QString execStr;
    bool ok;
    const auto &supportedActions = actions();

    while (!action.isEmpty() and !supportedActions.isEmpty()) {  // break trick
        if (auto index = supportedActions.indexOf(action); index == -1) {
            qWarning() << "can't find " << action << " in supported actions List. application will use default action to launch.";
            break;
        }

        const auto &actionHeader = QString{"%1%2"}.arg(DesktopFileEntryKey, action);
        const auto &actionExec = m_entry->value(actionHeader, "Exec");
        if (!actionExec) {
            break;
        }
        execStr = actionExec->toString(ok);
        if (!ok) {
            qWarning() << "exec value to string failed, try default action.";
            break;
        }
        break;
    }

    if (execStr.isEmpty()) {
        auto Actions = m_entry->value(DesktopFileEntryKey, "Exec");
        if (!Actions) {
            qWarning() << "application has isn't executable.";
            return {};
        }
        execStr = Actions->toString(ok);
        if (!ok) {
            qWarning() << "default action value to string failed. abort...";
            return {};
        }
    }

    auto [bin, cmds, res] = unescapeExec(execStr, fields);

    uid_t uid;
    auto it = options.find("uid");
    if (it != options.cend()) {
        uid = it->toUInt(&ok);
        if (!ok) {
            qWarning() << "convert uid string to uint failed: " << *it;
            return {};
        }

        if (uid != getCurrentUID()) {
            cmds.prepend(userNameLookup(uid));
            cmds.prepend("--user");
            cmds.prepend("pkexec");
        }
    } else {
        uid = getCurrentUID();
    }

    cmds.prepend("--");

    auto &jobManager = static_cast<ApplicationManager1Service *>(parent())->jobManager();

    return jobManager.addJob(
        m_applicationPath.path(),
        [this, binary = std::move(bin), commands = std::move(cmds)](QVariant variantValue) mutable -> QVariant {
            auto resourceFile = variantValue.toString();
            auto instanceRandomUUID = QUuid::createUuid().toString(QUuid::Id128);
            auto objectPath = m_applicationPath.path() + "/" + instanceRandomUUID;

            if (m_desktopSource.persistence()) {
                commands.push_front(QString{"--SourcePath=%1"}.arg(m_desktopSource.fileSource()));
            }

            if (resourceFile.isEmpty()) {
                commands.push_front(QString{R"(--unitName=app-DDE-%1@%2.service)"}.arg(
                    escapeApplicationId(this->id()), instanceRandomUUID));  // launcher should use this instanceId
                QProcess process;
                process.start(m_launcher, commands);
                process.waitForFinished();
                if (auto code = process.exitCode(); code != 0) {
                    qWarning() << "Launch Application Failed. exitCode:" << code;
                    return QString{""};
                }
                return objectPath;
            }

            qsizetype location{0};
            location = commands.indexOf(R"(%f)");
            if (location != -1) {  // due to std::move, there only remove once
                commands.remove(location);
            }

            auto url = QUrl::fromUserInput(resourceFile);
            if (!url.isValid()) {  // if url is invalid, passing to launcher directly
                auto scheme = url.scheme();
                if (!scheme.isEmpty()) {
                    // TODO: resourceFile = processRemoteFile(resourceFile);
                }
            }

            // NOTE: resourceFile must be available in the following contexts
            commands.insert(location, resourceFile);
            commands.push_front(QString{R"(--unitName=DDE-%1@%2.service)"}.arg(this->id(), instanceRandomUUID));
            QProcess process;
            process.start(getApplicationLauncherBinary(), commands);
            process.waitForFinished();
            auto exitCode = process.exitCode();
            if (exitCode != 0) {
                qWarning() << "Launch Application Failed:" << binary << commands;
                return QString{""};
            }
            return objectPath;
        },
        std::move(res));
}

QStringList ApplicationService::actions() const noexcept
{
    if (m_entry.isNull()) {
        qWarning() << "desktop entry is empty, isPersistence:" << m_desktopSource.persistence();
        return {};
    }

    const auto &actions = m_entry->value(DesktopFileEntryKey, "Actions");
    if (!actions) {
        return {};
    }

    bool ok{false};
    const auto &str = actions->toString(ok);
    if (!ok) {
        qWarning() << "Actions convert to String failed.";
        return {};
    }

    auto actionList = str.split(";");
    actionList.removeAll("");

    return actionList;
}

ObjectMap ApplicationService::GetManagedObjects() const
{
    return dumpDBusObject(m_Instances);
}

QString ApplicationService::id() const noexcept
{
    if (m_desktopSource.persistence()) {
        return m_desktopSource.desktopId();
    }
    return {};
}

IconMap ApplicationService::icons() const
{
    if (m_Icons) {
        return m_Icons->icons();
    }
    return {};
}

IconMap &ApplicationService::iconsRef()
{
    return m_Icons->iconsRef();
}

bool ApplicationService::isAutoStart() const noexcept
{
    return m_AutoStart;
}

void ApplicationService::setAutoStart(bool autostart) noexcept
{
    m_AutoStart = autostart;
}

QList<QDBusObjectPath> ApplicationService::instances() const noexcept
{
    return m_Instances.keys();
}

bool ApplicationService::addOneInstance(const QString &instanceId, const QString &application, const QString &systemdUnitPath)
{
    auto *service = new InstanceService{instanceId, application, systemdUnitPath};
    auto *adaptor = new InstanceAdaptor(service);
    QString objectPath{m_applicationPath.path() + "/" + instanceId};

    if (registerObjectToDBus(service, objectPath, getDBusInterface(QMetaType::fromType<InstanceAdaptor>()))) {
        m_Instances.insert(QDBusObjectPath{objectPath}, QSharedPointer<InstanceService>{service});
        service->moveToThread(this->thread());
        adaptor->moveToThread(this->thread());
        emit InterfacesAdded(QDBusObjectPath{objectPath}, getInterfacesListFromObject(service));
        return true;
    }

    adaptor->deleteLater();
    service->deleteLater();
    return false;
}

void ApplicationService::removeOneInstance(const QDBusObjectPath &instance) noexcept
{
    if (auto it = m_Instances.find(instance); it != m_Instances.cend()) {
        emit InterfacesRemoved(instance, getInterfacesListFromObject(it->data()));
        unregisterObjectFromDBus(instance.path());
        m_Instances.remove(instance);
    }
}

void ApplicationService::removeAllInstance() noexcept
{
    for (const auto &instance : m_Instances.keys()) {
        removeOneInstance(instance);
    }
}

QDBusObjectPath ApplicationService::findInstance(const QString &instanceId) const
{
    for (const auto &[path, ptr] : m_Instances.asKeyValueRange()) {
        if (ptr->instanceId() == instanceId) {
            return path;
        }
    }
    return {};
}

void ApplicationService::resetEntry(DesktopEntry *newEntry) noexcept
{
    m_entry.reset(newEntry);
}

QString ApplicationService::userNameLookup(uid_t uid)
{
    auto *pws = getpwuid(uid);
    return pws->pw_name;
}

LaunchTask ApplicationService::unescapeExec(const QString &str, const QStringList &fields)
{
    LaunchTask task;
    QStringList execList = str.split(' ');
    task.LaunchBin = execList.first();

    QRegularExpression re{"%[fFuUickdDnNvm]"};
    auto matcher = re.match(str);
    if (!matcher.hasMatch()) {
        task.command.append(std::move(execList));
        return task;
    }

    auto list = matcher.capturedTexts();
    if (list.count() > 1) {
        qWarning() << "invalid exec format, all filed code will be ignored.";
        for (const auto &code : list) {
            execList.removeOne(code);
        }
        task.command.append(std::move(execList));
        return task;
    }

    auto filesCode = list.first().back().toLatin1();
    auto codeStr = QString(R"(%%1)").arg(filesCode);
    auto location = execList.indexOf(codeStr);

    switch (filesCode) {
        case 'f': {  // Defer to async job
            task.command.append(std::move(execList));
            for (auto &field : fields) {
                task.Resources.emplace_back(std::move(field));
            }
            break;
        }
        case 'u': {
            execList.removeAt(location);
            if (fields.empty()) {
                task.command.append(execList);
                break;
            }
            if (fields.count() > 1) {
                qDebug() << R"(fields count is greater than one, %u will only take first element.)";
            }
            execList.insert(location, fields.first());
            task.command.append(execList);
            break;
        }
        case 'F':
            [[fallthrough]];
        case 'U': {
            execList.removeAt(location);
            auto it = execList.begin() + location;
            for (const auto &field : fields) {
                it = execList.insert(it, field);
            }
            task.command.append(std::move(execList));
            break;
        }
        case 'i': {
            execList.removeAt(location);
            auto val = m_entry->value(DesktopFileEntryKey, "Icon");
            if (!val) {
                qDebug() << R"(Application Icons can't be found. %i will be ignored.)";
                task.command.append(std::move(execList));
                return task;
            }
            bool ok;
            auto iconStr = val->toIconString(ok);
            if (!ok) {
                qDebug() << R"(Icons Convert to string failed. %i will be ignored.)";
                task.command.append(std::move(execList));
                return task;
            }
            auto it = execList.insert(location, iconStr);
            execList.insert(it, "--icon");
            task.command.append(std::move(execList));
            break;
        }
        case 'c': {
            execList.removeAt(location);
            auto val = m_entry->value(DesktopFileEntryKey, "Name");
            if (!val) {
                qDebug() << R"(Application Name can't be found. %c will be ignored.)";
                task.command.append(std::move(execList));
                return task;
            }
            bool ok;
            auto NameStr = val->toLocaleString(getUserLocale(), ok);
            if (!ok) {
                qDebug() << R"(Name Convert to locale string failed. %c will be ignored.)";
                task.command.append(std::move(execList));
                return task;
            }
            execList.insert(location, NameStr);
            task.command.append(std::move(execList));
            break;
        }
        case 'k': {  // ignore all desktop file location for now.
            execList.removeAt(location);
            task.command.append(std::move(execList));
            break;
        }
        case 'd':
        case 'D':
        case 'n':
        case 'N':
        case 'v':
            [[fallthrough]];  // Deprecated field codes should be removed from the command line and ignored.
        case 'm': {
            execList.removeAt(location);
            task.command.append(std::move(execList));
            break;
        }
    }

    if (task.Resources.isEmpty()) {
        task.Resources.emplace_back(QString{""});  // mapReduce should run once at least
    }

    return task;
}
