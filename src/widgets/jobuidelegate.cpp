/* This file is part of the KDE libraries
    Copyright (C) 2000 Stephan Kulow <coolo@kde.org>
                       David Faure <faure@kde.org>
    Copyright (C) 2006 Kevin Ottens <ervin@kde.org>
    Copyright (C) 2013 Dawit Alemayehu <adawit@kde.org>

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#include "jobuidelegate.h"
#include <kio/jobuidelegatefactory.h>
#include "kio_widgets_debug.h"

#include <kconfiggroup.h>
#include <kjob.h>
#include <kjobwidgets.h>
#include <klocalizedstring.h>
#include <kmessagebox.h>
#include <ksharedconfig.h>
#include <ksslinfodialog.h>
#include <clipboardupdater_p.h>

#include <QDBusInterface>
#include <QGuiApplication>
#include <QPointer>
#include <QWidget>
#include <QIcon>
#include <QUrl>

#include "kio/scheduler.h"

class Q_DECL_HIDDEN KIO::JobUiDelegate::Private
{
public:
};

KIO::JobUiDelegate::JobUiDelegate()
    : d(new Private())
{
}

KIO::JobUiDelegate::~JobUiDelegate()
{
    delete d;
}

/*
  Returns the top most window associated with widget.

  Unlike QWidget::window(), this function does its best to find and return the
  main application window associated with the given widget.

  If widget itself is a dialog or its parent is a dialog, and that dialog has a
  parent widget then this function will iterate through all those widgets to
  find the top most window, which most of the time is the main window of the
  application. By contrast, QWidget::window() would simply return the first
  file dialog it encountered since it is the "next ancestor widget that has (or
  could have) a window-system frame".
*/
static QWidget *topLevelWindow(QWidget *widget)
{
    QWidget *w = widget;
    while (w && w->parentWidget()) {
        w = w->parentWidget();
    }
    return (w ? w->window() : nullptr);
}

class JobUiDelegateStatic : public QObject
{
    Q_OBJECT
public:
    void registerWindow(QWidget *wid)
    {
        if (!wid) {
            return;
        }

        QWidget *window = topLevelWindow(wid);
        QObject *obj = static_cast<QObject *>(window);
        if (!m_windowList.contains(obj)) {
            // We must store the window Id because by the time
            // the destroyed signal is emitted we can no longer
            // access QWidget::winId() (already destructed)
            WId windowId = window->winId();
            m_windowList.insert(obj, windowId);
            connect(window, SIGNAL(destroyed(QObject*)),
                    this, SLOT(slotUnregisterWindow(QObject*)));
            QDBusInterface(QStringLiteral("org.kde.kded5"), QStringLiteral("/kded"), QStringLiteral("org.kde.kded5")).
            call(QDBus::NoBlock, QStringLiteral("registerWindowId"), qlonglong(windowId));
        }
    }
public Q_SLOTS:
    void slotUnregisterWindow(QObject *obj)
    {
        if (!obj) {
            return;
        }

        QMap<QObject *, WId>::Iterator it = m_windowList.find(obj);
        if (it == m_windowList.end()) {
            return;
        }
        WId windowId = it.value();
        disconnect(it.key(), SIGNAL(destroyed(QObject*)),
                   this, SLOT(slotUnregisterWindow(QObject*)));
        m_windowList.erase(it);
        QDBusInterface(QStringLiteral("org.kde.kded5"), QStringLiteral("/kded"), QStringLiteral("org.kde.kded5")).
        call(QDBus::NoBlock, QStringLiteral("unregisterWindowId"), qlonglong(windowId));
    }
private:
    QMap<QObject *, WId> m_windowList;
};

Q_GLOBAL_STATIC(JobUiDelegateStatic, s_static)

void KIO::JobUiDelegate::setWindow(QWidget *window)
{
    KDialogJobUiDelegate::setWindow(window);
    s_static()->registerWindow(window);
}

void KIO::JobUiDelegate::unregisterWindow(QWidget *window)
{
    s_static()->slotUnregisterWindow(window);
}

KIO::RenameDialog_Result KIO::JobUiDelegate::askFileRename(KJob *job,
        const QString &caption,
        const QUrl &src,
        const QUrl &dest,
        KIO::RenameDialog_Options options,
        QString &newDest,
        KIO::filesize_t sizeSrc,
        KIO::filesize_t sizeDest,
        const QDateTime &ctimeSrc,
        const QDateTime &ctimeDest,
        const QDateTime &mtimeSrc,
        const QDateTime &mtimeDest)
{
    //qDebug() << "job=" << job;
    // We now do it in process, so that opening the rename dialog
    // doesn't start uiserver for nothing if progressId=0 (e.g. F2 in konq)
    KIO::RenameDialog dlg(KJobWidgets::window(job), caption, src, dest, options,
                          sizeSrc, sizeDest,
                          ctimeSrc, ctimeDest, mtimeSrc,
                          mtimeDest);
    dlg.setWindowModality(Qt::WindowModal);
    connect(job, SIGNAL(finished(KJob*)), &dlg, SLOT(reject())); // #192976
    KIO::RenameDialog_Result res = static_cast<RenameDialog_Result>(dlg.exec());
    if (res == R_AUTO_RENAME) {
        newDest = dlg.autoDestUrl().path();
    } else {
        newDest = dlg.newDestUrl().path();
    }
    return res;
}

KIO::SkipDialog_Result KIO::JobUiDelegate::askSkip(KJob *job,
        KIO::SkipDialog_Options options,
        const QString &error_text)
{
    KIO::SkipDialog dlg(KJobWidgets::window(job), options, error_text);
    dlg.setWindowModality(Qt::WindowModal);
    connect(job, SIGNAL(finished(KJob*)), &dlg, SLOT(reject())); // #192976
    return static_cast<KIO::SkipDialog_Result>(dlg.exec());
}

bool KIO::JobUiDelegate::askDeleteConfirmation(const QList<QUrl> &urls,
        DeletionType deletionType,
        ConfirmationType confirmationType)
{
    QString keyName;
    bool ask = (confirmationType == ForceConfirmation);
    if (!ask) {
        KSharedConfigPtr kioConfig = KSharedConfig::openConfig(QStringLiteral("kiorc"), KConfig::NoGlobals);

        // The default value for confirmations is true for delete and false
        // for trash. If you change this, please also update:
        //      dolphin/src/settings/general/confirmationssettingspage.cpp
        bool defaultValue = true;

        switch (deletionType) {
        case Delete:
            keyName = QStringLiteral("ConfirmDelete");
            break;
        case Trash:
            keyName = QStringLiteral("ConfirmTrash");
            defaultValue = false;
            break;
        case EmptyTrash:
            keyName = QStringLiteral("ConfirmEmptyTrash");
            break;
        }

        ask = kioConfig->group("Confirmations").readEntry(keyName, defaultValue);
    }
    if (ask) {
        QStringList prettyList;
        Q_FOREACH (const QUrl &url, urls) {
            if (url.scheme() == QLatin1String("trash")) {
                QString path = url.path();
                // HACK (#98983): remove "0-foo". Note that it works better than
                // displaying KFileItem::name(), for files under a subdir.
                path.remove(QRegExp(QStringLiteral("^/[0-9]*-")));
                prettyList.append(path);
            } else {
                prettyList.append(url.toDisplayString(QUrl::PreferLocalFile));
            }
        }

        int result;
        QWidget *widget = window();
        const KMessageBox::Options options(KMessageBox::Notify | KMessageBox::WindowModal);
        switch (deletionType) {
        case Delete:
            if (prettyList.count() == 1) {
                result = KMessageBox::warningContinueCancel(
                            widget,
                            xi18nc("@info", "Do you really want to permanently delete this item?<nl/><filename>%1</filename><nl/><nl/><emphasis strong='true'>This action cannot be undone.</emphasis>", prettyList.first()),
                            QString(),
                            KStandardGuiItem::del(),
                            KStandardGuiItem::cancel(),
                            keyName, options);
            } else {
                result = KMessageBox::warningContinueCancelList(
                            widget,
                            xi18ncp("@info", "Do you really want to permanently delete this item?<nl/><nl/><emphasis strong='true'>This action cannot be undone.</emphasis>", "Do you really want to permanently delete these %1 items?<nl/><nl/><emphasis strong='true'>This action cannot be undone.</emphasis>", prettyList.count()),
                            prettyList,
                            i18n("Delete Files"),
                            KStandardGuiItem::del(),
                            KStandardGuiItem::cancel(),
                            keyName, options);
            }
            break;
        case EmptyTrash:
            result = KMessageBox::warningContinueCancel(
                         widget,
                         xi18nc("@info", "Do you want to permanently delete all items from the Trash?<nl/><nl/><emphasis strong='true'>This action cannot be undone.</emphasis>"),
                         QString(),
                         KGuiItem(i18nc("@action:button", "Empty Trash"),
                                  QIcon::fromTheme(QStringLiteral("user-trash"))),
                         KStandardGuiItem::cancel(),
                         keyName, options);
            break;
        case Trash:
        default:
            if (prettyList.count() == 1) {
                result = KMessageBox::warningContinueCancel(
                            widget,
                            xi18nc("@info", "Do you really want to move this item to the Trash?<nl/><filename>%1</filename>", prettyList.first()),
                            QString(),
                            KGuiItem(i18n("Move to Trash"), QStringLiteral("user-trash")),
                            KStandardGuiItem::cancel(),
                            keyName, options);
            } else {
                result = KMessageBox::warningContinueCancelList(
                            widget,
                            i18np("Do you really want to move this item to the trash?", "Do you really want to move these %1 items to the trash?", prettyList.count()),
                            prettyList,
                            i18n("Move to Trash"),
                            KGuiItem(i18n("Move to Trash"), QStringLiteral("user-trash")),
                            KStandardGuiItem::cancel(),
                            keyName, options);
            }
        }
        if (!keyName.isEmpty()) {
            // Check kmessagebox setting... erase & copy to konquerorrc.
            KSharedConfig::Ptr config = KSharedConfig::openConfig();
            KConfigGroup notificationGroup(config, "Notification Messages");
            if (!notificationGroup.readEntry(keyName, true)) {
                notificationGroup.writeEntry(keyName, true);
                notificationGroup.sync();

                KSharedConfigPtr kioConfig = KSharedConfig::openConfig(QStringLiteral("kiorc"), KConfig::NoGlobals);
                kioConfig->group("Confirmations").writeEntry(keyName, false);
            }
        }
        return (result == KMessageBox::Continue);
    }
    return true;
}

int KIO::JobUiDelegate::requestMessageBox(KIO::JobUiDelegate::MessageBoxType type,
        const QString &text, const QString &caption,
        const QString &buttonYes, const QString &buttonNo,
        const QString &iconYes, const QString &iconNo,
        const QString &dontAskAgainName,
        const KIO::MetaData &sslMetaData)
{
    int result = -1;

    //qDebug() << type << text << "caption=" << caption;

    KConfig config(QStringLiteral("kioslaverc"));
    KMessageBox::setDontShowAgainConfig(&config);

    const KGuiItem buttonYesGui(buttonYes, iconYes);
    const KGuiItem buttonNoGui(buttonNo, iconNo);
    KMessageBox::Options options(KMessageBox::Notify | KMessageBox::WindowModal);

    switch (type) {
    case QuestionYesNo:
        result = KMessageBox::questionYesNo(
                     window(), text, caption, buttonYesGui,
                     buttonNoGui, dontAskAgainName, options);
        break;
    case WarningYesNo:
        result = KMessageBox::warningYesNo(
                     window(), text, caption, buttonYesGui,
                     buttonNoGui, dontAskAgainName,
                     options | KMessageBox::Dangerous);
        break;
    case WarningYesNoCancel:
        result = KMessageBox::warningYesNoCancel(
                     window(), text, caption, buttonYesGui, buttonNoGui,
                     KStandardGuiItem::cancel(), dontAskAgainName, options);
        break;
    case WarningContinueCancel:
        result = KMessageBox::warningContinueCancel(
                     window(), text, caption, buttonYesGui,
                     KStandardGuiItem::cancel(), dontAskAgainName, options);
        break;
    case Information:
        KMessageBox::information(window(), text, caption, dontAskAgainName, options);
        result = 1; // whatever
        break;
    case SSLMessageBox: {
        QPointer<KSslInfoDialog> kid(new KSslInfoDialog(window()));
        //### this is boilerplate code and appears in khtml_part.cpp almost unchanged!
        const QStringList sl = sslMetaData.value(QStringLiteral("ssl_peer_chain")).split('\x01', QString::SkipEmptyParts);
        QList<QSslCertificate> certChain;
        bool decodedOk = true;
        foreach (const QString &s, sl) {
            certChain.append(QSslCertificate(s.toLatin1())); //or is it toLocal8Bit or whatever?
            if (certChain.last().isNull()) {
                decodedOk = false;
                break;
            }
        }

        if (decodedOk) {
            result = 1; // whatever
            kid->setSslInfo(certChain,
                            sslMetaData.value(QStringLiteral("ssl_peer_ip")),
                            text, // the URL
                            sslMetaData.value(QStringLiteral("ssl_protocol_version")),
                            sslMetaData.value(QStringLiteral("ssl_cipher")),
                            sslMetaData.value(QStringLiteral("ssl_cipher_used_bits")).toInt(),
                            sslMetaData.value(QStringLiteral("ssl_cipher_bits")).toInt(),
                            KSslInfoDialog::errorsFromString(sslMetaData.value(QStringLiteral("ssl_cert_errors"))));
            kid->exec();
        } else {
            result = -1;
            KMessageBox::information(window(),
                                     i18n("The peer SSL certificate chain appears to be corrupt."),
                                     i18n("SSL"), QString(), options);
        }
        // KSslInfoDialog deletes itself (Qt::WA_DeleteOnClose).
        delete kid;
        break;
    }
    default:
        qCWarning(KIO_WIDGETS) << "Unknown type" << type;
        result = 0;
        break;
    }
    KMessageBox::setDontShowAgainConfig(nullptr);
    return result;
}

KIO::ClipboardUpdater *KIO::JobUiDelegate::createClipboardUpdater(Job *job, ClipboardUpdaterMode mode)
{
    if (qobject_cast<QGuiApplication *>(qApp)) {
        return new KIO::ClipboardUpdater(job, mode);
    }
    return nullptr;
}

void KIO::JobUiDelegate::updateUrlInClipboard(const QUrl &src, const QUrl &dest)
{
    if (qobject_cast<QGuiApplication *>(qApp)) {
        KIO::ClipboardUpdater::update(src, dest);
    }
}

class KIOWidgetJobUiDelegateFactory : public KIO::JobUiDelegateFactory
{
public:
    KJobUiDelegate *createDelegate() const override
    {
        return new KIO::JobUiDelegate;
    }
};

Q_GLOBAL_STATIC(KIOWidgetJobUiDelegateFactory, globalUiDelegateFactory)
Q_GLOBAL_STATIC(KIO::JobUiDelegate, globalUiDelegate)

// Simply linking to this library, creates a GUI job delegate and delegate extension for all KIO jobs
static void registerJobUiDelegate()
{
    KIO::setDefaultJobUiDelegateFactory(globalUiDelegateFactory());
    KIO::setDefaultJobUiDelegateExtension(globalUiDelegate());
}

Q_CONSTRUCTOR_FUNCTION(registerJobUiDelegate)

#include "jobuidelegate.moc"
