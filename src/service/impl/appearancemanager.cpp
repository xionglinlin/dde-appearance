#include <fstream>
#include <regex>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <cstdlib>
#include <QRegExp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <pwd.h>
#include <QTimer>
#include <QTimeZone>
#include <stdio.h>
#include <QSettings>
#include <QColor>

#include "appearancemanager.h"
#include "appearancesyncconfig.h"
#include "modules/api/utils.h"
#include "modules/common/commondefine.h"
#include "modules/api/keyfile.h"
#include "dbus/appearancedbusproxy.h"

#include "modules/api/locale.h"
#include "modules/api/sunrisesunset.h"
#include "modules/api/themethumb.h"
#include "modules/dconfig/phasewallpaper.h"

#include "modules/subthemes/customtheme.h"

#define NAN_ANGLE (-200.0) // 异常经纬度

DCORE_USE_NAMESPACE

const QString wallpaperJsonPath = QString("%1/.cache/deepin/dde-appearance/").arg(getenv("HOME"));

AppearanceManager::AppearanceManager(QObject *parent)
    : QObject(parent)
    , settingDconfig(APPEARANCESCHEMA)
    , dbusProxy(new AppearanceDBusProxy(this))
//    , subthemes(new Subthemes(this))
    , backgrounds(new Backgrounds())
    , fontsManager(new FontsManager())
    , cursorChangeHandler(new CursorChangeHandler(this))
    , fsnotify(new Fsnotify())
    , detectSysClockTimer(this)
    , themeAutoTimer(this)
    , fontSize(0)
    , opacity(0)
    , windowRadius(0)
    , longitude(NAN_ANGLE) // 非法经纬度，未初始化状态
    , latitude(NAN_ANGLE)
    , ntpTimeId(0)
    , timeUpdateTimeId(0)
    , locationValid(false)
    , customTheme(new CustomTheme())
    , globalThemeUpdating(false)
    , workspaceCount(0)
{
    if (QGSettings::isSchemaInstalled(XSETTINGSSCHEMA)) {
        xSetting = QSharedPointer<QGSettings>(new QGSettings(XSETTINGSSCHEMA));
    }

    if (QGSettings::isSchemaInstalled(WRAPBGSCHEMA)) {
        wrapBgSetting = QSharedPointer<QGSettings>(new QGSettings(WRAPBGSCHEMA));
    }

    if (QGSettings::isSchemaInstalled(XSETTINGSSCHEMA)) {
        gnomeBgSetting = QSharedPointer<QGSettings>(new QGSettings(GNOMEBGSCHEMA));
    }

    if (wrapBgSetting) {
        background = wrapBgSetting->get(GSKEYBACKGROUND).toString();
    }

    if (settingDconfig.isValid()) {
        globalTheme = settingDconfig.value(GSKEYGLOBALTHEME).toString();
        gtkTheme = settingDconfig.value(GSKEYGTKTHEME).toString();
        iconTheme = settingDconfig.value(GSKEYICONTHEM).toString();
        cursorTheme = settingDconfig.value(GSKEYCURSORTHEME).toString();
        standardFont = settingDconfig.value(GSKEYFONTSTANDARD).toString();
        monospaceFont = settingDconfig.value(GSKEYFONTMONOSPACE).toString();
        fontSize = settingDconfig.value(GSKEYFONTSIZE).toDouble();
        opacity = settingDconfig.value(GSKEYOPACITY).toDouble();
        wallpaperSlideShow = settingDconfig.value(GSKEYWALLPAPERSLIDESHOW).toString();
        wallpaperURls = settingDconfig.value(GSKEYWALLPAPERURIS).toString();
    }

    if (xSetting) {
        windowRadius = xSetting->get(GSKEYDTKWINDOWRADIUS).toInt();
        qtActiveColor = qtActiveColorToHexColor(xSetting->get(GSKEYQTACTIVECOLOR).toString());
    }

    init();
}

AppearanceManager::~AppearanceManager()
{
    delete customTheme;
    customTheme = nullptr;
}

bool AppearanceManager::init()
{
    qInfo() << "init";
    getScaleFactor();
    // subthemes需要在获取ScaleFactor后再初始化
    subthemes.reset(new Subthemes(this));

    initCoordinate();
    initUserObj();
    initCurrentBgs();

    xcb_connection_t *conn = xcb_connect(nullptr, nullptr);
    xcb_randr_query_version_reply_t *v = xcb_randr_query_version_reply(conn, xcb_randr_query_version(conn, 1, 1), nullptr);
    if (v == nullptr) {
        qWarning() << "xcb_randr_query_version_reply faile";
        return false;
    }

    connect(dbusProxy.get(), &AppearanceDBusProxy::workspaceCountChanged, this, &AppearanceManager::handleWmWorkspaceCountChanged);
    workspaceCount = dbusProxy->WorkspaceCount();
    connect(dbusProxy.get(), &AppearanceDBusProxy::WorkspaceSwitched, this, &AppearanceManager::handleWmWorkspaceSwithched);
    connect(dbusProxy.get(), &AppearanceDBusProxy::SetScaleFactorStarted, this, &AppearanceManager::handleSetScaleFactorStarted);
    connect(dbusProxy.get(), &AppearanceDBusProxy::SetScaleFactorDone, this, &AppearanceManager::handleSetScaleFactorDone);

    connect(dbusProxy.get(), &AppearanceDBusProxy::PrimaryChanged, this, &AppearanceManager::updateMonitorMap);
    connect(dbusProxy.get(), &AppearanceDBusProxy::MonitorsChanged, this, &AppearanceManager::updateMonitorMap);

    updateMonitorMap();

    new ThemeFontSyncConfig("org.deepin.daemon.Appearance1", "/org/deepin/dde/Appearance1/sync", QSharedPointer<AppearanceManager>(this));
    new BackgroundSyncConfig("org.deepin.daemon.Appearance1", "/org/deepin/dde/Appearance1/Background", QSharedPointer<AppearanceManager>(this));

    if (wallpaperURls.isEmpty()) {
        updateNewVersionData();
    }

    zone = dbusProxy->timezone();

    connect(dbusProxy.get(), &AppearanceDBusProxy::TimezoneChanged, this, &AppearanceManager::handleTimezoneChanged);
    connect(dbusProxy.get(), &AppearanceDBusProxy::NTPChanged, this, &AppearanceManager::handleTimeUpdate);

    loadDefaultFontConfig();
    connect(dbusProxy.get(), &AppearanceDBusProxy::TimeUpdate, this, &AppearanceManager::handleTimeUpdate);
    connect(dbusProxy.get(), &AppearanceDBusProxy::NTPSessionChanged, this, &AppearanceManager::handleNTPChanged);

    QVector<QSharedPointer<Theme>> globalList = subthemes->listGlobalThemes();
    bool bFound = false;

    for (auto theme : globalList) {
        if (theme->getId() == globalTheme) {
            bFound = true;
            break;
        }
    }
    if (!bFound) {
        setGlobalTheme(DEFAULTICONTHEME);
        doSetGlobalTheme(DEFAULTICONTHEME);
    }

    QVector<QSharedPointer<Theme>> iconList = subthemes->listIconThemes();
    bFound = false;

    for (auto theme : iconList) {
        if (theme->getId() == iconTheme) {
            bFound = true;
            break;
        }
    }
    if (!bFound) {
        setIconTheme(DEFAULTICONTHEME);
        doSetIconTheme(DEFAULTICONTHEME);
    }

    QVector<QSharedPointer<Theme>> cursorList = subthemes->listCursorThemes();
    bFound = false;
    for (auto theme : cursorList) {
        if (theme->getId() == cursorTheme) {
            bFound = true;
            break;
        }
    }
    if (!bFound) {
        setCursorTheme(DEFAULTCURSORTHEME);
        doSetCursorTheme(DEFAULTCURSORTHEME);
    }

    cursorChangeHandler->start();

    connect(fsnotify.data(), SIGNAL(themeFileChange(QString)), this, SLOT(handlethemeFileChange(QString)));

    connect(xSetting.data(), SIGNAL(changed(const QString &)), this, SLOT(handleXsettingDConfigChange(QString)));

    connect(&settingDconfig, SIGNAL(valueChanged(const QString &)), this, SLOT(handleSettingDConfigChange(QString)));

    connect(wrapBgSetting.data(), SIGNAL(changed(const QString)), this, SLOT(handleWrapBgDConfigChange(QString)));

    connect(gnomeBgSetting.data(), SIGNAL(changed(const QString)), this, SLOT(handleGnomeBgDConfigChange(QString)));

    connect(&detectSysClockTimer, SIGNAL(timeout()), this, SLOT(handleDetectSysClockTimeOut()));
    connect(&themeAutoTimer, SIGNAL(timeout()), this, SLOT(handleGlobalThemeChangeTimeOut()));
    themeAutoTimer.start(60000); // 每分钟检查一次是否要切换主题

    connect(customTheme, &CustomTheme::updateToCustom, this, &AppearanceManager::handleUpdateToCustom);

    return true;
}

void AppearanceManager::deleteThermByType(const QString &ty, const QString &name)
{
    if (ty.compare(TYPEGTK) == 0) {
        subthemes->deleteGtkTheme(name);
    } else if (ty.compare(TYPEICON) == 0) {
        subthemes->deleteIconTheme(name);
    } else if (ty.compare(TYPECURSOR) == 0) {
        subthemes->deleteCursorTheme(name);
    } else if (ty.compare(TYPEBACKGROUND) == 0) {
        backgrounds->deleteBackground(name);
    } else {
        // todo log
    }
}
void AppearanceManager::handleWmWorkspaceCountChanged(int count)
{
    workspaceCount = count;
    QStringList bgs = settingDconfig.value(GSKEYBACKGROUNDURIS).toStringList();

    if (bgs.size() < count) {
        QVector<Background> allBgs = backgrounds->listBackground();

        int addCount = count - bgs.count();
        for (int i = 0; i < addCount; i++) {
            int index = rand() % allBgs.count();

            bgs.push_back(allBgs[index].getId());
        }

        settingDconfig.setValue(GSKEYBACKGROUNDURIS, bgs);
    } else if (bgs.size() > count) {
        bgs = bgs.mid(0, count);
        settingDconfig.setValue(GSKEYBACKGROUNDURIS, bgs);
    }

    doUpdateWallpaperURIs();
}

void AppearanceManager::handleWmWorkspaceSwithched(int from, int to)
{
    dbusProxy->SetCurrentWorkspace(to);
}

void AppearanceManager::handleSetScaleFactorStarted()
{
    QString body = tr("Setting display scaling");
    QString summary = tr("Display scaling");
    dbusProxy->Notify("dde-control-center", "dialog-window-scale", summary, body, {}, {}, 0);
}

void AppearanceManager::handleSetScaleFactorDone()
{
    QString body = tr("Log out for display scaling settings to take effect");
    QString summary = tr("Set successfully");
    QStringList options{ "_logout", tr("Log Out Now"), "_later", tr("Later") };
    QMap<QString, QVariant> optionMap;
    optionMap["x-deepin-action-_logout"] = "dbus-send,--type=method_call,--dest=org.deepin.dde.SessionManager1,"
                                           "/org/deepin/dde/SessionManager1,org.deepin.dde.SessionManager1.RequestLogout";
    optionMap["x-deepin-action-_later"] = "";
    int expireTimeout = 15 * 1000;
    dbusProxy->Notify("dde-control-center", "dialog-window-scale", summary, body, options, optionMap, expireTimeout);
    // 更新ScaleFactor缓存
    getScaleFactor();
}

void AppearanceManager::handleTimezoneChanged(QString timezone)
{
    if (coordinateMap.count(timezone) == 1) {
        latitude = coordinateMap[timezone].latitude;
        longitude = coordinateMap[timezone].longitude;
    }
    zone = timezone;
    // todo l, err := time.LoadLocation(zone)

    if (gtkTheme == AUTOGTKTHEME) {
        autoSetTheme(latitude, longitude);
        resetThemeAutoTimer();
    }
}

void AppearanceManager::handleTimeUpdate()
{
    locationValid = true;
    timeUpdateTimeId = this->startTimer(2000);
}

void AppearanceManager::handleNTPChanged()
{
    locationValid = true;
    ntpTimeId = this->startTimer(2000);
}

void AppearanceManager::handlethemeFileChange(QString theme)
{
    if (theme == TYPEGLOBALTHEME) {
        subthemes->refreshGlobalThemes();
        Q_EMIT Refreshed(TYPEGLOBALTHEME);
    } else if (theme == TYPEBACKGROUND) {
        backgrounds->notifyChanged();
        for (auto iter : wsLoopMap) {
            iter->notifyFileChange();
        }
    } else if (theme == TYPEGTK) {
        // todo <-time.After(time.Millisecond * 700)
        subthemes->refreshGtkThemes();
        Q_EMIT Refreshed(TYPEGTK);
    } else if (theme == TYPEICON) {
        // todo <-time.After(time.Millisecond * 700)
        subthemes->refreshIconThemes();
        subthemes->refreshCursorThemes();
        Q_EMIT Refreshed(TYPEICON);
        Q_EMIT Refreshed(TYPECURSOR);
    }
}

void AppearanceManager::handleXsettingDConfigChange(QString key)
{
    if (key == GSKEYQTACTIVECOLOR) {
        QString value = qtActiveColorToHexColor(xSetting->get(GSKEYQTACTIVECOLOR).toString());

        qtActiveColor = value;
        Q_EMIT Changed("QtActiveColor", value);
    } else if (key == GSKEYDTKWINDOWRADIUS) {
        windowRadius = xSetting->get(GSKEYDTKWINDOWRADIUS).toInt();
        Q_EMIT Changed("WindowRadius", QString::number(windowRadius));
    }
}

void AppearanceManager::handleSettingDConfigChange(QString key)
{
    QString type;
    QString value;
    bool bSuccess = false;
    if (key == GSKEYGLOBALTHEME) {
        type = TYPEGLOBALTHEME;
        value = settingDconfig.value(key).toString();
        bSuccess = doSetGlobalTheme(value);
    } else if (key == GSKEYGTKTHEME) {
        type = TYPEGTK;
        value = settingDconfig.value(key).toString();
        bSuccess = doSetGtkTheme(value);
    } else if (key == GSKEYICONTHEM) {
        type = TYPEICON;
        value = settingDconfig.value(key).toString();
        bSuccess = doSetIconTheme(value);
    } else if (key == GSKEYCURSORTHEME) {
        type = TYPECURSOR;
        value = settingDconfig.value(key).toString();
        bSuccess = doSetCursorTheme(value);
    } else if (key == GSKEYFONTSTANDARD) {
        type = TYPESTANDARDFONT;
        value = settingDconfig.value(key).toString();
        bSuccess = doSetStandardFont(value);
    } else if (key == GSKEYFONTMONOSPACE) {
        type = TYPEMONOSPACEFONT;
        value = settingDconfig.value(key).toString();
        bSuccess = doSetMonospaceFont(value);
    } else if (key == GSKEYFONTSIZE) {
        type = TYPEFONTSIZE;
        double size = settingDconfig.value(key).toDouble();
        bSuccess = doSetFonts(size);
        value = QString::number(size);
    } else if (key == GSKEYBACKGROUNDURIS) {
        type = TYPEBACKGROUND;
        desktopBgs = settingDconfig.value(key).toStringList();
        dbusProxy->SetDesktopBackgrounds(desktopBgs);
        value = desktopBgs.join(";");
    } else if (key == GSKEYWALLPAPERSLIDESHOW) {
        type = TYPEWALLPAPERSLIDESHOW;
        value = settingDconfig.value(key).toString();
        updateWSPolicy(value);
    } else if (key == GSKEYOPACITY) {
        type = TYPEWINDOWOPACITY;
        bool ok = false;
        double opacity = settingDconfig.value(key).toDouble(&ok);
        if (ok) {
            setOpacity(opacity);
            value = QString::number(opacity);
        }
    } else if (key == DCKEYALLWALLPAPER) {
        type = TYPEALLWALLPAPER;
        value = settingDconfig.value(key).toString();
        QDir dir;
        if (!dir.exists(wallpaperJsonPath)) {
            bool res = dir.mkpath(wallpaperJsonPath);
            if (!res) {
                qWarning() << QString("mkdir %1 failed.").arg(wallpaperJsonPath);
                return;
            }
        }

        QFile file(wallpaperJsonPath + "config.json");
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream textStream(&file);
            textStream << QString(QJsonDocument::fromVariant(value).toJson());
            textStream.flush();
            file.close();
        } else {
            qWarning() << QString("%1 error.").arg(wallpaperJsonPath);
        }
    } else {
        return;
    }

    if (!bSuccess) {
        qDebug() << "set " << key << "fail";
    }

    if (!type.isEmpty()) {
        Q_EMIT Changed(type, value);
    }
}

void AppearanceManager::handleWrapBgDConfigChange(QString key)
{
    if (key != GSKEYBACKGROUND) {
        return;
    }

    QString value = wrapBgSetting->get(key).toString();
    bool bSuccess = doSetBackground(value);
    if (!bSuccess) {
        return;
    }

    if (wsLoopMap.count(curMonitorSpace) != 0) {
        wsLoopMap[curMonitorSpace]->addToShow(value);
    }
}

void AppearanceManager::handleGnomeBgDConfigChange(QString key)
{
    if (key != GSKEYBACKGROUND) {
        return;
    }

    QString value = gnomeBgSetting->get(key).toString();
    bool bSuccess = doSetBackground(value);
    if (!bSuccess) {
        return;
    }

    if (wsLoopMap.count(curMonitorSpace) != 0) {
        wsLoopMap[curMonitorSpace]->addToShow(value);
    }
}

void AppearanceManager::handleDetectSysClockTimeOut()
{

    qint64 now = QDateTime::currentSecsSinceEpoch();
    qint64 diff = now - detectSysClockStartTime - 60;
    if (diff > -2 && diff < 2) {
        if (locationValid) {
            autoSetTheme(latitude, longitude);
            resetThemeAutoTimer();
        }
        detectSysClockStartTime = QDateTime::currentSecsSinceEpoch();
        detectSysClockTimer.start(60 * 1000);
    }
}

void AppearanceManager::handleUpdateToCustom(const QString &mode)
{
    currentGlobalTheme = "custom" + mode;
    setGlobalTheme("custom");
}

void AppearanceManager::handleGlobalThemeChangeTimeOut()
{
    // 相同则为指定主题
    if (globalTheme == currentGlobalTheme
        || longitude <= NAN_ANGLE
        || latitude <= NAN_ANGLE)
        return;
    autoSetTheme(latitude, longitude);
}

void AppearanceManager::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == timeUpdateTimeId || event->timerId() == ntpTimeId) {
        if (locationValid) {
            autoSetTheme(latitude, longitude);
            resetThemeAutoTimer();
        }

        killTimer(event->timerId());
    }
}
// 设置gsetting
void AppearanceManager::setFontSize(double value)
{
    if (!fontsManager->isFontSizeValid(value)) {
        qWarning() << "set font size error:invalid size " << value;
        return;
    }

    if (settingDconfig.isValid() && !qFuzzyCompare(value, fontSize)) {
        settingDconfig.setValue(GSKEYFONTSIZE, value);
        fontSize = value;
        updateCustomTheme(TYPEFONTSIZE, QString::number(value));
    }
}

void AppearanceManager::setGlobalTheme(QString value)
{
    if (settingDconfig.isValid() && value != globalTheme) {
        settingDconfig.setValue(GSKEYGLOBALTHEME, value);
        globalTheme = value;
    }
}

void AppearanceManager::setGtkTheme(QString value)
{
    if (settingDconfig.isValid() && value != gtkTheme) {
        settingDconfig.setValue(GSKEYGTKTHEME, value);
        gtkTheme = value;
    }
}

void AppearanceManager::setIconTheme(QString value)
{
    if (settingDconfig.isValid() && value != iconTheme) {
        settingDconfig.setValue(GSKEYICONTHEM, value);
        iconTheme = value;
    }
}

void AppearanceManager::setCursorTheme(QString value)
{
    if (settingDconfig.isValid() && value != cursorTheme) {
        settingDconfig.setValue(GSKEYCURSORTHEME, value);
        cursorTheme = value;
    }
}

void AppearanceManager::setStandardFont(QString value)
{
    if (!fontsManager->isFontFamily(value)) {
        qWarning() << "set standard font error:invalid font " << value;
        return;
    }

    if (settingDconfig.isValid() && value != standardFont) {
        settingDconfig.setValue(GSKEYFONTSTANDARD, value);
        standardFont = value;
    }
}

void AppearanceManager::setMonospaceFont(QString value)
{
    if (!fontsManager->isFontFamily(value)) {
        qWarning() << "set monospace font error:invalid font " << value;
        return;
    }

    if (settingDconfig.isValid() && value != monospaceFont) {
        settingDconfig.setValue(GSKEYFONTMONOSPACE, value);
        monospaceFont = value;
    }
}

void AppearanceManager::setWindowRadius(int value)
{
    if (value != windowRadius && xSetting) {
        xSetting->set(GSKEYDTKWINDOWRADIUS, value);
        windowRadius = value;
        updateCustomTheme(TYPWINDOWRADIUS, QString::number(value));
    }
}

void AppearanceManager::setOpacity(double value)
{
    if (settingDconfig.isValid() && !qFuzzyCompare(value, opacity)) {
        settingDconfig.setValue(GSKEYOPACITY, value);
        opacity = value;
        updateCustomTheme(TYPEWINDOWOPACITY, QString::number(value));
    }
}

void AppearanceManager::setQtActiveColor(const QString &value)
{
    if (value != qtActiveColor && xSetting) {
        xSetting->set(GSKEYQTACTIVECOLOR, hexColorToQtActiveColor(value));
        qtActiveColor = value;
        updateCustomTheme(TYPEACTIVECOLOR, value);
    }
}

bool AppearanceManager::setWallpaperSlideShow(const QString &value)
{
    if (value == wallpaperSlideShow) {
        return true;
    }
    if (!settingDconfig.isValid()) {
        return false;
    }
    qInfo() << "value: " << value;
    qInfo() << "value: GSKEYWALLPAPERSLIDESHOW" << settingDconfig.value(GSKEYWALLPAPERSLIDESHOW);
    settingDconfig.setValue(GSKEYWALLPAPERSLIDESHOW, value);
    wallpaperSlideShow = value;

    return true;
}

bool AppearanceManager::setWallpaperURls(const QString &value)
{
    if (value == wallpaperURls) {
        return true;
    }
    if (!settingDconfig.isValid()) {
        return false;
    }

    settingDconfig.setValue(GSKEYWALLPAPERURIS, value);
    wallpaperURls = value;

    return true;
}

QString AppearanceManager::qtActiveColorToHexColor(const QString &activeColor) const
{
    QStringList fields = activeColor.split(",");
    if (fields.size() != 4)
        return "";

    QColor clr = QColor::fromRgba64(fields.at(0).toUShort(), fields.at(1).toUShort(), fields.at(2).toUShort(), fields.at(3).toUShort());
    return clr.name(clr.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb).toUpper();
}

QString AppearanceManager::hexColorToQtActiveColor(const QString &hexColor) const
{
    if (!QColor::isValidColor(hexColor))
        return QString();
    QColor clr(hexColor);
    QStringList rgbaList;
    QRgba64 clr64 = clr.rgba64();
    rgbaList.append(QString::number(clr64.red()));
    rgbaList.append(QString::number(clr64.green()));
    rgbaList.append(QString::number(clr64.blue()));
    rgbaList.append(QString::number(clr64.alpha()));
    return rgbaList.join(",");
}

void AppearanceManager::initCoordinate()
{
    QString context;
    QFile file(ZONEPATH);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }

    while (!file.atEnd()) {
        QString line = file.readLine();
        if (line.length() == 0) {
            continue;
        }
        line = line.trimmed();
        if (line.startsWith("#")) {
            continue;
        }

        QStringList strv = line.split("\t");
        if (strv.size() < 3) {
            continue;
        }

        iso6709Parsing(strv[2], strv[1]);
    }

    QString city = dbusProxy->timezone();
    if (coordinateMap.count(city) == 1) {
        latitude = coordinateMap[city].latitude;
        longitude = coordinateMap[city].longitude;
    }
}

void AppearanceManager::initUserObj()
{
    qInfo() << "initUserObj";
    struct passwd *pw = getpwuid(getuid());
    if (pw == nullptr) {
        return;
    }

    QString userPath = AppearanceDBusProxy::FindUserById(QString::number(pw->pw_uid));

    dbusProxy->setUserInterface(userPath);

    QStringList userBackgrounds = dbusProxy->desktopBackgrounds();

    QStringList gsBackgrounds = settingDconfig.value(GSKEYBACKGROUNDURIS).toStringList();
    for (auto iter : gsBackgrounds) {
        if (userBackgrounds.indexOf(iter) == -1) {
            dbusProxy->SetDesktopBackgrounds(gsBackgrounds);
            break;
        }
    }
}

void AppearanceManager::initCurrentBgs()
{
    qInfo() << "initCurrentBgs";
    desktopBgs = settingDconfig.value(GSKEYBACKGROUNDURIS).toStringList();

    greeterBg = dbusProxy->greeterBackground();
}

void AppearanceManager::initWallpaperSlideshow()
{
    loadWSConfig();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(wallpaperSlideShow.toLatin1(), &err);
    if (err.error != QJsonParseError::NoError) {
        qWarning() << "parse wallpaperSlideShow: " << wallpaperSlideShow << ",fail";
        return;
    }

    QVariantMap tempMap = doc.object().toVariantMap();
    for (auto iter : tempMap.toStdMap()) {
        if (wsSchedulerMap.count(iter.first) != 1) {
            QSharedPointer<WallpaperScheduler> wallpaperScheduler(new WallpaperScheduler(std::bind(
                    &AppearanceManager::autoChangeBg, this, std::placeholders::_1, std::placeholders::_2)));
            wsSchedulerMap[iter.first] = wallpaperScheduler;
        }

        if (wsLoopMap.count(iter.second.toString()) != 1) {
            wsLoopMap[iter.first] = QSharedPointer<WallpaperLoop>(new WallpaperLoop());
        }

        if (WallpaperLoopConfigManger::isValidWSPolicy(iter.second.toString())) {
            if (iter.second.toString() == WSPOLICYLOGIN) {
                bool bSuccess = changeBgAfterLogin(iter.first);
                if (!bSuccess) {
                    qWarning() << "failed to change background after login";
                }
            } else {
                uint sec = iter.second.toString().toUInt();
                if (wsSchedulerMap.count(iter.first) == 1) {
                    wsSchedulerMap[iter.first]->setInterval(iter.first, sec);
                }
            }
        }
    }
}

void AppearanceManager::updateMonitorMap()
{
    QString primary = dbusProxy->primary();
    QStringList monitorList = dbusProxy->ListOutputNames();
    for (int i = 0; i < monitorList.size(); i++) {
        if (monitorList[i] == primary) {
            monitorMap[monitorList[i]] = "Primary";
        } else {
            monitorMap[monitorList[i]] = "Subsidiary" + QString::number(i);
        }
    }
}

void AppearanceManager::iso6709Parsing(QString city, QString coordinates)
{
    QRegExp pattern("(\\+|-)\\d+\\.?\\d*");

    QVector<QString> resultVet;

    int pos = 0;
    while ((pos = pattern.indexIn(coordinates, pos)) != -1 && resultVet.size() <= 2) {
        resultVet.push_back(coordinates.mid(pos, pattern.matchedLength()));
        pos += pattern.matchedLength();
    }

    if (resultVet.size() < 2) {
        return;
    }

    resultVet[0] = resultVet[0].mid(0, 3) + "." + resultVet[0].mid(3, resultVet[0].size());
    resultVet[1] = resultVet[1].mid(0, 4) + "." + resultVet[1].mid(4, resultVet[1].size());

    coordinate cdn;

    cdn.latitude = resultVet[0].toDouble();
    cdn.longitude = resultVet[1].toDouble();

    coordinateMap[city] = cdn;
}

void AppearanceManager::doUpdateWallpaperURIs()
{
    QMap<QString, QString> monitorWallpaperUris;

    QStringList monitorList = dbusProxy->ListOutputNames();

    for (int i = 0; i < monitorList.length(); i++) {
        for (int idx = 1; idx <= workspaceCount; idx++) {
            QString wallpaperUri = dbusProxy->GetWorkspaceBackgroundForMonitor(idx, monitorList.at(i));
            if (wallpaperUri.isEmpty())
                continue;

            QString key;
            if (monitorMap.count(monitorList[i]) != 0) {
                key = QString::asprintf("%s&&%d", monitorMap[monitorList[i]].toLatin1().data(), idx);
            } else {
                key = QString::asprintf("&&%d", idx);
            }

            monitorWallpaperUris[key] = wallpaperUri;
        }
    }

    setPropertyWallpaperURIs(monitorWallpaperUris);
}

void AppearanceManager::setPropertyWallpaperURIs(QMap<QString, QString> monitorWallpaperUris)
{
    QJsonDocument doc;
    QJsonObject monitorObj;
    for (auto iter : monitorWallpaperUris.toStdMap()) {
        monitorObj.insert(iter.first, iter.second);
    }

    doc.setObject(monitorObj);
    QString wallPaperUriVal = doc.toJson(QJsonDocument::Compact);

    settingDconfig.setValue(GSKEYWALLPAPERURIS, wallPaperUriVal);
}

void AppearanceManager::updateNewVersionData()
{
    QString primaryMonitor;
    for (auto item : monitorMap.toStdMap()) {
        if (item.second == "Primary") {
            primaryMonitor = item.first;
        }
    }
    QJsonDocument doc = QJsonDocument::fromJson(wallpaperSlideShow.toLatin1());
    QJsonObject wallPaperSlideObj;
    if (!doc.isEmpty()) {
        for (int i = 1; i <= workspaceCount; i++) {
            QString key = QString("%1&&%2").arg(primaryMonitor).arg(i);
            wallPaperSlideObj.insert(key, wallpaperSlideShow);
        }

        QJsonDocument tempDoc(wallPaperSlideObj);

        if (!setWallpaperSlideShow(tempDoc.toJson(QJsonDocument::Compact))) {
            return;
        }
    }

    QJsonObject wallpaperURIsObj;
    for (auto item : monitorMap.toStdMap()) {
        for (int i = 1; i <= workspaceCount; i++) {
            QString wallpaperURI = dbusProxy->GetWorkspaceBackgroundForMonitor(i, item.first);
            if (wallpaperURI.isEmpty())
                continue;
            QString key = QString("%1&&%2").arg(item.second).arg(i);
            wallpaperURIsObj.insert(key, wallpaperURI);
        }
    }

    QJsonDocument tempDoc(wallpaperURIsObj);
    setWallpaperURls(tempDoc.toJson(QJsonDocument::Compact));
}

void AppearanceManager::autoSetTheme(double latitude, double longitude)
{
    QDateTime curr = QDateTime::currentDateTimeUtc();
    curr.setTimeZone(QTimeZone(zone.toLatin1()));
    double utcOffset = curr.offsetFromUtc() / 3600.0;

    QDateTime sunrise, sunset;
    bool bSuccess = SunriseSunset::getSunriseSunset(latitude, longitude, utcOffset, curr.date(), sunrise, sunset);
    if (!bSuccess) {
        return;
    }
    QString themeName;
    curr = QDateTime::currentDateTimeUtc();
    if (sunrise.secsTo(curr) >= 0 && curr.secsTo(sunset) >= 0) {
        themeName = globalTheme + ".light";
    } else {
        themeName = globalTheme + ".dark";
    }

    if (currentGlobalTheme != themeName) {
        doSetGlobalTheme(themeName);
    }
}

void AppearanceManager::resetThemeAutoTimer()
{
    if (!locationValid) {
        qDebug() << "location is invalid";
        return;
    }

    QDateTime curr = QDateTime::currentDateTime();
    QDateTime changeTime = getThemeAutoChangeTime(curr, latitude, longitude);

    qint64 interval = curr.msecsTo(changeTime);
    qDebug() << "change theme after:" << interval << curr << changeTime;
}

void AppearanceManager::loadDefaultFontConfig()
{
    QFile file(DEFAULTFONTCONFIGFILE);
    if (!file.open(QIODevice::ReadOnly)) {
        return;
    }

    QJsonParseError jsonError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &jsonError);
    if (jsonError.error != QJsonParseError::NoError) {
        qDebug() << "json error!" << jsonError.errorString();
        return;
    }

    QVariantMap rootMap = doc.object().toVariantMap();
    for (auto item : rootMap.toStdMap()) {
        AppearanceManager::fontConfigItem fontConfig;
        QVariantMap itemMap = item.second.toJsonObject().toVariantMap();
        if (!itemMap.value("Standard").toString().isEmpty()) {
            fontConfig.Standard = itemMap.value("Standard").toString();
        }

        if (!itemMap.value("Mono").toString().isEmpty()) {
            fontConfig.Monospace = itemMap.value("Mono").toString();
        }

        defaultFontConfigMap[item.first] = fontConfig;
    }
}

void AppearanceManager::getDefaultFonts(QString &standard, QString &monospace)
{
    if (defaultFontConfigMap.empty()) {
        standard = DEFAULTSTANDARDFONT;
        monospace = DEFAULTMONOSPACEFONT;
        return;
    }

    QVector<QString> languages = Locale::instance()->getLanguageNames();
    for (auto lang : languages) {
        if (defaultFontConfigMap.count(lang) != 0) {
            standard = defaultFontConfigMap[lang].Standard;
            monospace = defaultFontConfigMap[lang].Monospace;
            return;
        }
    }

    if (defaultFontConfigMap.count("en_US") == 1) {
        standard = defaultFontConfigMap["en_US"].Standard;
        monospace = defaultFontConfigMap["en_US"].Monospace;
    }
    return;
}

void AppearanceManager::updateThemeAuto(bool enable)
{
    enableDetectSysClock(enable);

    if (enable) {
        QString city = dbusProxy->timezone();
        if (coordinateMap.count(city) == 1) {
            latitude = coordinateMap[city].latitude;
            longitude = coordinateMap[city].longitude;
        }
        locationValid = true;
        autoSetTheme(latitude, longitude);
        resetThemeAutoTimer();
    }
}

void AppearanceManager::enableDetectSysClock(bool enable)
{
    if (enable) {
        detectSysClockTimer.start(60 * 1000);
    } else {
        detectSysClockTimer.stop();
    }
}

void AppearanceManager::updateWSPolicy(QString policy)
{
    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(policy.toLatin1(), &error);
    if (error.error != QJsonParseError::NoError) {
        return;
    }
    loadWSConfig();

    QVariantMap config = doc.object().toVariantMap();
    for (auto iter : config.toStdMap()) {
        if (wsSchedulerMap.count(iter.first) == 0) {
            QSharedPointer<WallpaperScheduler> wallpaperScheduler(new WallpaperScheduler(std::bind(
                    &AppearanceManager::autoChangeBg, this, std::placeholders::_1, std::placeholders::_2)));
            wsSchedulerMap[iter.first] = wallpaperScheduler;
        }

        if (wsLoopMap.count(iter.first) == 0) {
            wsLoopMap[iter.first] = QSharedPointer<WallpaperLoop>(new WallpaperLoop());
        }

        if (curMonitorSpace == iter.first && WallpaperLoopConfigManger::isValidWSPolicy(policy)) {
            bool bOk;
            int nSec = policy.toInt(&bOk);
            if (bOk) {
                wsSchedulerMap[iter.first]->setLastChangeTime(QDateTime::currentDateTime());
                wsSchedulerMap[iter.first]->setInterval(iter.first, nSec);
                saveWSConfig(iter.first, QDateTime::currentDateTime());
            } else {
                wsSchedulerMap[iter.first]->stop();
            }
        }
    }
}

void AppearanceManager::loadWSConfig()
{
    WallpaperLoopConfigManger wallConfig;
    QString fileName = utils::GetUserConfigDir() + "/deepin/dde-daemon/appearance/wallpaper-slideshow.json";
    WallpaperLoopConfigManger::WallpaperLoopConfigMap cfg = wallConfig.loadWSConfig(fileName);

    for (auto monitorSpace : cfg.keys()) {
        if (wsSchedulerMap.count(monitorSpace) == 0) {
            QSharedPointer<WallpaperScheduler> wallpaperScheduler(new WallpaperScheduler(std::bind(&AppearanceManager::autoChangeBg, this, std::placeholders::_1, std::placeholders::_2)));
            wsSchedulerMap[monitorSpace] = wallpaperScheduler;
        }

        wsSchedulerMap[monitorSpace]->setLastChangeTime(cfg[monitorSpace].lastChange);

        if (wsLoopMap.count(monitorSpace) == 0) {
            wsLoopMap[monitorSpace] = QSharedPointer<WallpaperLoop>(new WallpaperLoop());
        }

        for (auto file : cfg[monitorSpace].showedList) {
            wsLoopMap[monitorSpace]->addToShow(file);
        }
    }
}

QDateTime AppearanceManager::getThemeAutoChangeTime(QDateTime date, double latitude, double longitude)
{
    QDateTime curr = QDateTime::currentDateTime();

    double utcOffset = curr.offsetFromUtc() / 3600.0;

    QDateTime sunrise, sunset;
    bool bSuccess = SunriseSunset::getSunriseSunset(latitude, longitude, utcOffset, curr.date(), sunrise, sunset);
    if (!bSuccess) {
        return QDateTime();
    }

    if (curr.secsTo(sunrise) > 0) {
        return sunrise;
    }

    if (curr.secsTo(sunset) > 0) {
        return sunset;
    }

    curr = curr.addDays(1);

    bSuccess = SunriseSunset::getSunriseSunset(latitude, longitude, utcOffset, curr.date(), sunrise, sunset);
    if (!bSuccess) {
        return QDateTime();
    }

    return sunrise;
}

bool AppearanceManager::doSetFonts(double size)
{
    if (!fontsManager->isFontSizeValid(size)) {
        qWarning() << "set font size error:invalid size " << size;
        return false;
    }

    qDebug() << "doSetFonts, standardFont:" << standardFont << ", monospaceFont:" << monospaceFont;
    bool bSuccess = fontsManager->setFamily(standardFont, monospaceFont, size);
    if (!bSuccess) {
        qWarning() << "set font size error:can not set family ";
        return false;
    }
    dbusProxy->SetString("Qt/FontPointSize", QString::number(size));
    if (!setDQtTheme({ QTKEYFONTSIZE }, { QString::number(size) })) {
        qWarning() << "set font size error:can not set qt theme ";
        return false;
    }

    return true;
}

bool AppearanceManager::doSetGlobalTheme(QString value)
{
    enum GolbalThemeMode {
        Light = 1,
        Dark = 2,
        Auto = 3,
    };
    QString themeId = value;
    GolbalThemeMode mode = Auto;
    if (value.endsWith(".light")) {
        themeId = value.chopped(6);
        mode = Light;
    } else if (value.endsWith(".dark")) {
        themeId = value.chopped(5);
        mode = Dark;
    }

    QVector<QSharedPointer<Theme>> globalThemes = subthemes->listGlobalThemes();
    QString themePath;
    for (auto iter : globalThemes) {
        if (iter->getId() == themeId) {
            themePath = iter->getPath();
            break;
        }
    }
    if (themePath.isEmpty())
        return false;

    KeyFile theme(',');
    theme.loadFile(themePath + "/index.theme");
    QString defaultTheme = theme.getStr("Deepin Theme", "DefaultTheme");
    if (defaultTheme.isEmpty())
        return false;
    QString darkTheme = theme.getStr("Deepin Theme", "DarkTheme");
    if (darkTheme.isEmpty() && mode == Auto)
        mode = Light;
    switch (mode) {
    case Light:
        applyGlobalTheme(theme, defaultTheme, defaultTheme);
        currentGlobalTheme = value;
        break;
    case Dark: {
        if (darkTheme.isEmpty())
            return false;
        applyGlobalTheme(theme, darkTheme, defaultTheme);
        currentGlobalTheme = value;
    } break;
    case Auto: {
        updateThemeAuto(true);
    } break;
    }

    return true;
}

bool AppearanceManager::doSetGtkTheme(QString value)
{
    if (value == AUTOGTKTHEME) {
        return true;
    }

    if (!subthemes->isGtkTheme(value)) {
        return false;
    }
    QString ddeKWinTheme;
    if (value == DEEPIN) {
        ddeKWinTheme = "light";
    } else if (value == DEEPINDARK) {
        ddeKWinTheme = "dark";
    }

    if (!ddeKWinTheme.isEmpty()) {
        dbusProxy->SetDecorationDeepinTheme(ddeKWinTheme);
    }
    return subthemes->setGtkTheme(value);
}

bool AppearanceManager::doSetIconTheme(QString value)
{
    if (!subthemes->isIconTheme(value)) {
        return false;
    }

    if (!subthemes->setIconTheme(value)) {
        return false;
    }

    return setDQtTheme({ QTKEYICON }, { value });
}

bool AppearanceManager::doSetCursorTheme(QString value)
{
    if (!subthemes->isCursorTheme(value)) {
        return false;
    }

    return subthemes->setCursorTheme(value);
}

bool AppearanceManager::doSetStandardFont(QString value)
{
    if (!fontsManager->isFontFamily(value)) {
        qWarning() << "set standard font error:invalid font " << value;
        return false;
    }
    QString tmpMonoFont = monospaceFont;
    QStringList fontList = fontsManager->listMonospace();
    if (!fontList.isEmpty()) {
        tmpMonoFont = fontList[0];
    }

    qDebug() << "doSetStandardFont standardFont:" << standardFont << ", monospaceFont:" << monospaceFont;
    if (!fontsManager->setFamily(value, tmpMonoFont, fontSize)) {
        qWarning() << "set standard font error:can not set family ";
        return false;
    }
    dbusProxy->SetString("Qt/FontName", value);
    if (!setDQtTheme({ QTKEYFONT }, { value })) {
        qWarning() << "set standard font error:can not set qt theme ";
        return false;
    }
    return true;
}

bool AppearanceManager::doSetMonospaceFont(QString value)
{
    if (!fontsManager->isFontFamily(value)) {
        return false;
    }
    QString tmpStandardFont = standardFont;
    QStringList fontList = fontsManager->listStandard();
    if (!fontList.isEmpty()) {
        tmpStandardFont = fontList[0];
    }

    qDebug() << "doSetMonospaceFont, standardFont:" << standardFont << ", monospaceFont:" << monospaceFont;
    if (!fontsManager->setFamily(tmpStandardFont, value, fontSize)) {
        qWarning() << "set monospace font error:can not set family ";
        return false;
    }

    dbusProxy->SetString("Qt/MonoFontName", value);
    if (!setDQtTheme({ QTKEYMONOFONT }, { value })) {
        qWarning() << "set monospace font error:can not set qt theme ";
        return false;
    }

    return true;
}

bool AppearanceManager::doSetBackground(QString value)
{
    if (!backgrounds->isBackgroundFile(value)) {
        return false;
    }

    QString file = backgrounds->prepare(value);

    QString uri = utils::enCodeURI(file, SCHEME_FILE);

    dbusProxy->ChangeCurrentWorkspaceBackground(uri);

    dbusProxy->Get(file);

    dbusProxy->Get("", file);

    return true;
}

bool AppearanceManager::doSetGreeterBackground(QString value)
{
    value = utils::enCodeURI(value, SCHEME_FILE);
    greeterBg = value;
    dbusProxy->SetGreeterBackground(value);
    return true;
}

QString AppearanceManager::doGetWallpaperSlideShow(QString monitorName)
{
    int index = dbusProxy->GetCurrentWorkspace();

    QJsonDocument doc = QJsonDocument::fromJson(wallpaperSlideShow.toLatin1());
    QVariantMap tempMap = doc.object().toVariantMap();

    QString key = QString("%1&&%2").arg(monitorName).arg(index);

    if (tempMap.count(key) == 1) {
        return tempMap[key].toString();
    }

    return "";
}

double AppearanceManager::getScaleFactor()
{
    double scaleFactor = dbusProxy->GetScaleFactor();
    qInfo()<<__FUNCTION__<<"UpdateScaleFactor"<<scaleFactor;
    UpdateScaleFactor(scaleFactor);
    return scaleFactor;
}

ScaleFactors AppearanceManager::getScreenScaleFactors()
{
    return dbusProxy->GetScreenScaleFactors();
}

bool AppearanceManager::setScaleFactor(double scale)
{
    dbusProxy->SetScaleFactor(scale);
    return true;
}

bool AppearanceManager::setScreenScaleFactors(ScaleFactors scaleFactors)
{
    dbusProxy->SetScreenScaleFactors(scaleFactors);
    return true;
}

QString AppearanceManager::doList(QString type)
{
    if (type == TYPEGTK) {
        QVector<QSharedPointer<Theme>> gtks = subthemes->listGtkThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = gtks.begin();
        while (iter != gtks.end()) {
            if ((*iter)->getId().startsWith("deepin")) {
                ++iter;
            } else {
                iter = gtks.erase(iter);
            }
        }
        return marshal(gtks);
    } else if (type == TYPEICON) {
        return marshal(subthemes->listIconThemes());
    } else if (type == TYPECURSOR) {
        return marshal(subthemes->listCursorThemes());
    } else if (type == TYPEBACKGROUND) {
        return marshal(backgrounds->listBackground());
    } else if (type == TYPESTANDARDFONT) {
        return marshal(fontsManager->listStandard());
    } else if (type == TYPEMONOSPACEFONT) {
        return marshal(fontsManager->listMonospace());
    } else if (type == TYPEGLOBALTHEME) {
        return marshal(subthemes->listGlobalThemes());
    }

    return "";
}

QString AppearanceManager::doShow(const QString &type, const QStringList &names)
{
    if (type == TYPEGTK) {
        QVector<QSharedPointer<Theme>> gtks = subthemes->listGtkThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = gtks.begin();
        while (iter != gtks.end()) {
            if (names.indexOf((*iter)->getId()) != -1 || (*iter)->getId() == AUTOGTKTHEME) {
                ++iter;
            } else {
                iter = gtks.erase(iter);
            }
        }
        return marshal(gtks);
    } else if (type == TYPEICON) {
        QVector<QSharedPointer<Theme>> icons = subthemes->listIconThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = icons.begin();
        while (iter != icons.end()) {
            if (names.indexOf((*iter)->getId()) != -1) {
                ++iter;
            } else {
                iter = icons.erase(iter);
            }
        }
        return marshal(icons);
    } else if (type == TYPEGLOBALTHEME) {
        QVector<QSharedPointer<Theme>> globalThemes = subthemes->listGlobalThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = globalThemes.begin();
        while (iter != globalThemes.end()) {
            if (names.indexOf((*iter)->getId()) != -1) {
                ++iter;
            } else {
                iter = globalThemes.erase(iter);
            }
        }
        return marshal(globalThemes);
    } else if (type == TYPECURSOR) {
        QVector<QSharedPointer<Theme>> cursor = subthemes->listCursorThemes();

        QVector<QSharedPointer<Theme>>::iterator iter = cursor.begin();
        while (iter != cursor.end()) {
            if (names.indexOf((*iter)->getId()) != -1) {
                ++iter;
            } else {
                iter = cursor.erase(iter);
            }
        }
        return marshal(cursor);
    } else if (type == TYPEBACKGROUND) {
        QVector<Background> background = backgrounds->listBackground();

        QVector<Background>::iterator iter = background.begin();
        while (iter != background.end()) {
            if (names.indexOf(iter->getId()) != -1) {
                ++iter;
            } else {
                iter = background.erase(iter);
            }
        }
        return marshal(background);
    } else if (type == TYPESTANDARDFONT) {
        return marshal(fontsManager->getFamilies(names));
    } else if (type == TYPEMONOSPACEFONT) {
        return marshal(fontsManager->getFamilies(names));
    }

    return "";
}

void AppearanceManager::doResetSettingBykeys(QStringList keys)
{
    QStringList keyList = settingDconfig.keyList();
    for (auto item : keys) {
        if (keyList.contains(item)) {
            continue;
        }
        settingDconfig.reset(item);
    }
}

void AppearanceManager::doResetFonts()
{
    QString defaultStandardFont, defaultMonospaceFont;
    getDefaultFonts(defaultStandardFont, defaultMonospaceFont);

    if (defaultStandardFont != standardFont) {
        setStandardFont(defaultStandardFont);
    }

    if (defaultMonospaceFont != monospaceFont) {
        setMonospaceFont(defaultMonospaceFont);
    }

    bool bSuccess = fontsManager->setFamily(defaultStandardFont, defaultMonospaceFont, fontSize);
    if (!bSuccess) {
        return;
    }

    fontsManager->checkFontConfVersion();
}

void AppearanceManager::doSetByType(const QString &type, const QString &value)
{
    bool updateValut = false;
    if (type == TYPEGTK) {
        if (value == gtkTheme) {
            return;
        }

        if (doSetGtkTheme(value)) {
            setGtkTheme(value);
            updateValut = true;
        }
    } else if (type == TYPEICON) {
        if (value == iconTheme) {
            return;
        }

        if (doSetIconTheme(value)) {
            setIconTheme(value);
            updateValut = true;
        }
    } else if (type == TYPECURSOR) {
        if (value == cursorTheme) {
            return;
        }

        if (doSetCursorTheme(value)) {
            setCursorTheme(value);
            updateValut = true;
        }
    } else if (type == TYPEGLOBALTHEME) {
        if (value == globalTheme) {
            return;
        }
        if (doSetGlobalTheme(value)) {
            setGlobalTheme(value);
        }
    } else if (type == TYPEBACKGROUND) {
        bool bSuccess = doSetBackground(value);
        if (bSuccess && wsLoopMap.count(curMonitorSpace) == 1) {
            wsLoopMap[curMonitorSpace]->addToShow(value);
            updateValut = true;
        }
    } else if (type == TYPEGREETERBACKGROUND) {
        updateValut = doSetGreeterBackground(value);
    } else if (type == TYPESTANDARDFONT) {
        if (standardFont == value) {
            return;
        }
        setStandardFont(value);
        updateValut = true;
    } else if (type == TYPEMONOSPACEFONT) {
        if (monospaceFont == value) {
            return;
        }
        setMonospaceFont(value);
        updateValut = true;
    } else if (type == TYPEFONTSIZE) {
        double size = value.toDouble();
        if (fontSize > size - 0.01 && fontSize < size + 0.01) {
            return;
        }
        setFontSize(size);
    } else if (type == TYPEACTIVECOLOR) {
        setQtActiveColor(value);
    } else if (type == TYPWINDOWRADIUS) {
        bool ok = false;
        int radius = value.toInt(&ok);
        if (ok) {
            setWindowRadius(radius);
        }
    } else if (type == TYPEWINDOWOPACITY) {
        bool ok = false;
        double opacity = value.toDouble(&ok);
        if (ok) {
            setOpacity(opacity);
        }
    }
    if (updateValut) {
        updateCustomTheme(type, value);
    }
}

QString AppearanceManager::doSetMonitorBackground(const QString &monitorName, const QString &imageGile)
{
    if (!backgrounds->isBackgroundFile(imageGile)) {
        return "";
    }

    QString file = backgrounds->prepare(imageGile);

    QString uri = utils::enCodeURI(file, SCHEME_FILE);

    dbusProxy->SetCurrentWorkspaceBackgroundForMonitor(uri, monitorName);

    doUpdateWallpaperURIs();

    dbusProxy->Get(file);

    dbusProxy->Get("", file);

    return file;
}

QString AppearanceManager::doThumbnail(const QString &type, const QString &name)
{
    if (type == TYPEGTK) {
        QMap<QString, QString> gtkThumbnailMap = subthemes->getGtkThumbnailMap();
        if (gtkThumbnailMap.count(name) == 1) {
            return "/usr/share/dde-daemon/appearance/" + gtkThumbnailMap[name] + ".svg";
        }
        return subthemes->getGtkThumbnail(name);
    } else if (type == TYPEICON) {
        return subthemes->getIconThumbnail(name);
    } else if (type == TYPECURSOR) {
        return subthemes->getCursorThumbnail(name);
    } else if (type == TYPEGLOBALTHEME) {
        return subthemes->getGlobalThumbnail(name);
    } else {
        return QString("invalid type: %1").arg(type);
    }
}

bool AppearanceManager::doSetWallpaperSlideShow(const QString &monitorName, const QString &wallpaperSlideShow)
{
    int idx = dbusProxy->GetCurrentWorkspace();

    QJsonDocument doc = QJsonDocument::fromJson(wallpaperSlideShow.toLatin1());
    QJsonObject cfgObj = doc.object();

    QString key = QString("%1&&%2").arg(monitorName).arg(idx);

    cfgObj[key] = wallpaperSlideShow;

    QJsonDocument docTmp;
    docTmp.setObject(cfgObj);
    QString value = docTmp.toJson(QJsonDocument::Compact);

    setWallpaperSlideShow(value);

    curMonitorSpace = key;

    return true;
}

bool AppearanceManager::doSetWsLoop(const QString &monitorName, const QString &file)
{
    int index = dbusProxy->GetCurrentWorkspace();
    QString monitor = QString("%1&&%2").arg(monitorName).arg(index);
    if (wsLoopMap.count(monitor) == 1) {
        wsLoopMap[monitor]->addToShow(file);
    }

    return true;
}

int AppearanceManager::getCurrentDesktopIndex()
{
    return dbusProxy->GetCurrentWorkspace();
}

void AppearanceManager::applyGlobalTheme(KeyFile &theme, const QString &themeName, const QString &defaultTheme)
{
    globalThemeUpdating = true;
    QString defTheme = (defaultTheme.isEmpty() || defaultTheme == themeName) ? QString() : defaultTheme;
    // 设置globlaTheme的一项，先从themeName中找对应项，若没有则从defTheme中找对应项，最后调用doSetByType实现功能
    auto setGlobalItem = [&theme, &themeName, &defTheme, this](const QString &key, const QString &type) {
        QString themeValue = theme.getStr(themeName, key);
        if (themeValue.isEmpty() && !defTheme.isEmpty())
            themeValue = theme.getStr(defTheme, key);
        if (!themeValue.isEmpty())
            doSetByType(type, themeValue);
    };

    setGlobalItem("Wallpaper", TYPEBACKGROUND);
    setGlobalItem("LockBackground", TYPEGREETERBACKGROUND);
    setGlobalItem("IconTheme", TYPEICON);
    setGlobalItem("CursorTheme", TYPECURSOR);
    setGlobalItem("AppTheme", TYPEGTK);
    setGlobalItem("StandardFont", TYPESTANDARDFONT);
    setGlobalItem("MonospaceFont", TYPEMONOSPACEFONT);
    setGlobalItem("FontSize", TYPEFONTSIZE);
    setGlobalItem("ActiveColor", TYPEACTIVECOLOR);
    //    setGlobalItem("DockBackground", TYPESTANDARDFONT);
    //    setGlobalItem("DockOpacity", TYPEDOCKOPACITY);
    //    setGlobalItem("LauncherBackground", TYPEBACKGROUND);
    //    setGlobalItem("LauncherOpacity", TYPESTANDARDFONT);
    setGlobalItem("WindowRadius", TYPWINDOWRADIUS);
    setGlobalItem("WindowOpacity", TYPEWINDOWOPACITY);
    globalThemeUpdating = false;
}

void AppearanceManager::updateCustomTheme(const QString &type, const QString &value)
{
    if (!globalThemeUpdating) {
        customTheme->updateValue(type, value, globalTheme, subthemes->listGlobalThemes());
    }
}

void AppearanceManager::doSetCurrentWorkspaceBackground(const QString &uri)
{
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm";
        return;
    }

    PhaseWallPaper::setWallpaperUri(strIndex, "", uri);
    return;
}

QString AppearanceManager::doGetCurrentWorkspaceBackground()
{
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm.";
        return "";
    }

    return PhaseWallPaper::getWallpaperUri(strIndex, "");
}

void AppearanceManager::doSetCurrentWorkspaceBackgroundForMonitor(const QString &uri, const QString &strMonitorName)
{
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm";
        return;
    }

    PhaseWallPaper::setWallpaperUri(strIndex, strMonitorName, uri);
    return;
}

QString AppearanceManager::doGetCurrentWorkspaceBackgroundForMonitor(const QString &strMonitorName)
{
    QString strIndex = QString::number(getCurrentDesktopIndex());
    if (strIndex == "") {
        qWarning() << "error getting current desktop index through wm.";
        return "";
    }

    return PhaseWallPaper::getWallpaperUri(strIndex, strMonitorName);
}

void AppearanceManager::doSetWorkspaceBackgroundForMonitor(const int &index, const QString &strMonitorName, const QString &uri)
{
    return PhaseWallPaper::setWallpaperUri(QString::number(index), strMonitorName, uri);
}

QString AppearanceManager::doGetWorkspaceBackgroundForMonitor(const int &index, const QString &strMonitorName)
{
    return PhaseWallPaper::getWallpaperUri(QString::number(index), strMonitorName);
}

void AppearanceManager::autoChangeBg(QString monitorSpace, QDateTime date)
{
    qDebug() << "autoChangeBg: " << monitorSpace << ", " << date;

    if (wsLoopMap.count(monitorSpace) == 0) {
        return;
    }

    QString file = wsLoopMap[monitorSpace]->getNext();
    if (file.isEmpty()) {
        qDebug() << "file is empty";
        return;
    }

    QString strIndex = QString::number(getCurrentDesktopIndex());

    int index = monitorSpace.indexOf("&&");
    if (index == -1) {
        qWarning() << "monitorSpace format error";
        return;
    }

    if (strIndex == monitorSpace.right(index + 2)) {
        doSetMonitorBackground(monitorSpace.left(index), file);
    }

    saveWSConfig(monitorSpace, date);
}

bool AppearanceManager::changeBgAfterLogin(QString monitorSpace)
{
    QString runDir = g_get_user_runtime_dir();

    QFile file("/proc/self/sessionid");
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "open /proc/self/sessionid fail";
        return false;
    }

    QString currentSessionId = file.readAll();
    currentSessionId = currentSessionId.simplified();

    bool needChangeBg = false;
    runDir = runDir + "/dde-daemon-wallpaper-slideshow-login" + "/" + monitorSpace;
    QFile fileTemp(runDir);

    if (!file.exists()) {
        needChangeBg = true;
    } else if (!fileTemp.open(QIODevice::ReadOnly)) {
        qWarning() << "open " << runDir << " fail";
        return false;
    } else {
        if (currentSessionId != fileTemp.readAll().simplified()) {
            needChangeBg = true;
        }
    }

    if (needChangeBg) {
        autoChangeBg(monitorSpace, QDateTime::currentDateTime());
        fileTemp.write(currentSessionId.toLatin1());
    }

    file.close();
    fileTemp.close();

    return true;
}

bool AppearanceManager::setDQtTheme(QStringList key, QStringList value)
{
    if (key.length() != value.length()) {
        return false;
    }

    QString filePath = utils::GetUserConfigDir() + "/deepin" + "/qt-theme.ini";

    QSettings settings(filePath, QSettings::IniFormat);
    settings.beginGroup("Theme");
    for (int i = 0; i < key.length(); i++) {
        QString temp = settings.value(key[i]).value<QString>();
        if (temp == value[i]) {
            continue;
        }
        settings.setValue(key[i], value[i]);
    }

    return true;
}

bool AppearanceManager::saveWSConfig(QString monitorSpace, QDateTime date)
{
    WallpaperLoopConfigManger configManger;

    QString fileName = utils::GetUserConfigDir() + "/deepin/dde-daemon/appearance/wallpaper-slideshow.json";
    configManger.loadWSConfig(fileName);

    if (wsLoopMap.count(monitorSpace) != 0) {
        configManger.setShowed(monitorSpace, wsLoopMap[monitorSpace]->getShowed());
    }
    configManger.setLastChange(monitorSpace, date);

    return configManger.save(fileName);
}

QString AppearanceManager::marshal(const QVector<QSharedPointer<Theme>> &themes)
{
    QJsonDocument doc;
    QJsonArray array;
    for (auto iter : themes) {
        QJsonObject obj;
        obj.insert("Id", iter->getId());
        obj.insert("Path", iter->getPath());
        obj.insert("Deletable", iter->getDeleteable());
        obj.insert("Name", iter->name());
        obj.insert("Comment", iter->comment());
        obj.insert("hasDark", iter->hasDark());
        obj.insert("Example", iter->example());
        array.append(obj);
    }

    doc.setArray(array);

    return doc.toJson(QJsonDocument::Compact);
}

QString AppearanceManager::marshal(const QVector<Background> &backgrounds)
{
    QJsonDocument doc;
    QJsonArray array;
    for (auto iter : backgrounds) {
        QJsonObject obj;
        obj.insert("Id", iter.getId());
        obj.insert("Deletable", iter.getDeleteable());
        array.append(obj);
    }

    doc.setArray(array);

    return doc.toJson(QJsonDocument::Compact);
}

QString AppearanceManager::marshal(const QStringList &strs)
{
    QJsonDocument doc;
    QJsonArray array;
    for (auto iter : strs) {
        array.append(iter);
    }

    doc.setArray(array);

    return doc.toJson(QJsonDocument::Compact);
}

QString AppearanceManager::marshal(const QVector<QSharedPointer<FontsManager::Family>> &strs)
{
    QJsonDocument doc;

    QJsonArray arr;
    for (auto iter : strs) {
        QJsonObject obj;
        obj["Id"] = iter->id;
        obj["Name"] = iter->id;
        obj["Styles"] = iter->id;
        obj["Show"] = iter->id;
        arr.push_back(obj);
    }

    doc.setArray(arr);
    return doc.toJson(QJsonDocument::Compact);
}
