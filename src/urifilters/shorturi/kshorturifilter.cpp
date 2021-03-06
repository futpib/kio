/*  -*- c-basic-offset: 2 -*-

    kshorturifilter.h

    This file is part of the KDE project
    Copyright (C) 2000 Dawit Alemayehu <adawit@kde.org>
    Copyright (C) 2000 Malte Starostik <starosti@zedat.fu-berlin.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "kshorturifilter.h"
#include "../../pathhelpers_p.h"

#include <QDir>
#include <QtDBus>
#include <QRegularExpression>
#include <qplatformdefs.h>

#include <klocalizedstring.h>
#include <kpluginfactory.h>
#include <kprotocolinfo.h>
#include <kconfig.h>
#include <kconfiggroup.h>
#include <kmimetypetrader.h>
#include <kservice.h>
#include <kurlauthorized.h>
#include <kuser.h>

namespace {
QLoggingCategory category("org.kde.kurifilter-shorturi", QtWarningMsg);
}

/**
 * IMPORTANT: If you change anything here, make sure you run the kurifiltertest
 * regression test (this should be included as part of "make test").
 *
 * If you add anything, make sure to extend kurifiltertest to make sure it is
 * covered.
 */

typedef QMap<QString,QString> EntryMap;

static QRegularExpression sEnvVarExp (QStringLiteral("\\$[a-zA-Z_][a-zA-Z0-9_]*"));

static bool isPotentialShortURL(const QString& cmd)
{
    // Host names and IPv4 address...
    if (cmd.contains(QLatin1Char('.'))) {
        return true;
    }

    // IPv6 Address...
    if (cmd.startsWith(QLatin1Char('[')) && cmd.contains(QLatin1Char(':'))) {
        return true;
    }

    return false;
}

static QString removeArgs( const QString& _cmd )
{
  QString cmd( _cmd );

  if( cmd[0] != '\'' && cmd[0] != '"' )
  {
    // Remove command-line options (look for first non-escaped space)
    int spacePos = 0;

    do
    {
      spacePos = cmd.indexOf( ' ', spacePos+1 );
    } while ( spacePos > 1 && cmd[spacePos - 1] == '\\' );

    if( spacePos > 0 )
    {
      cmd = cmd.left( spacePos );
      qCDebug(category) << "spacePos=" << spacePos << " returning " << cmd;
    }
  }

  return cmd;
}

static bool isKnownProtocol(const QString &protocol)
{
    if (KProtocolInfo::isKnownProtocol(protocol) || protocol == QLatin1String("mailto")) {
        return true;
    }
    const KService::Ptr service = KMimeTypeTrader::self()->preferredService(QLatin1String("x-scheme-handler/") + protocol);
    return service;
}

KShortUriFilter::KShortUriFilter( QObject *parent, const QVariantList & /*args*/ )
                :KUriFilterPlugin( QStringLiteral("kshorturifilter"), parent )
{
    QDBusConnection::sessionBus().connect(QString(), QStringLiteral("/"), QStringLiteral("org.kde.KUriFilterPlugin"),
                                QStringLiteral("configure"), this, SLOT(configure()));
    configure();
}

bool KShortUriFilter::filterUri( KUriFilterData& data ) const
{
 /*
  * Here is a description of how the shortURI deals with the supplied
  * data.  First it expands any environment variable settings and then
  * deals with special shortURI cases. These special cases are the "smb:"
  * URL scheme which is very specific to KDE, "#" and "##" which are
  * shortcuts for man:/ and info:/ protocols respectively. It then handles
  * local files.  Then it checks to see if the URL is valid and one that is
  * supported by KDE's IO system.  If all the above checks fails, it simply
  * lookups the URL in the user-defined list and returns without filtering
  * if it is not found. TODO: the user-defined table is currently only manually
  * hackable and is missing a config dialog.
  */

  //QUrl url = data.uri();
  QString cmd = data.typedString();

  int firstNonSlash = 0;
  while (firstNonSlash < cmd.length() && (cmd.at(firstNonSlash) == '/')) {
      firstNonSlash++;
  }
  if (firstNonSlash > 1) {
      cmd = cmd.mid(firstNonSlash - 1);
  }

  // Replicate what KUrl(cmd) did in KDE4. This could later be folded into the checks further down...
  QUrl url;
  if (QDir::isAbsolutePath(cmd)) {
    url = QUrl::fromLocalFile(cmd);
  } else {
    url.setUrl(cmd);
  }

  // WORKAROUND: Allow the use of '@' in the username component of a URL since
  // other browsers such as firefox in their infinite wisdom allow such blatant
  // violations of RFC 3986. BR# 69326/118413.
  if (cmd.count(QLatin1Char('@')) > 1) {
    const int lastIndex = cmd.lastIndexOf(QLatin1Char('@'));
    // Percent encode all but the last '@'.
    QString encodedCmd = QUrl::toPercentEncoding(cmd.left(lastIndex), ":/");
    encodedCmd += cmd.midRef(lastIndex);
    cmd = encodedCmd;
    url.setUrl(encodedCmd);
  }

  const bool isMalformed = !url.isValid();
  QString protocol = url.scheme();

  qCDebug(category) << cmd;

  // Fix misparsing of "foo:80", QUrl thinks "foo" is the protocol and "80" is the path.
  // However, be careful not to do that for valid hostless URLs, e.g. file:///foo!
  if (!protocol.isEmpty() && url.host().isEmpty() && !url.path().isEmpty()
      && cmd.contains(':') && !isKnownProtocol(protocol)) {
    protocol.clear();
  }

  qCDebug(category) << "url=" << url << "cmd=" << cmd << "isMalformed=" << isMalformed;

  // TODO: Make this a bit more intelligent for Minicli! There
  // is no need to make comparisons if the supplied data is a local
  // executable and only the argument part, if any, changed! (Dawit)
  // You mean caching the last filtering, to try and reuse it, to save stat()s? (David)

  const QString starthere_proto = QStringLiteral("start-here:");
  if (cmd.indexOf(starthere_proto) == 0 )
  {
    setFilteredUri( data, QUrl(QStringLiteral("system:/")) );
    setUriType( data, KUriFilterData::LocalDir );
    return true;
  }

  // Handle MAN & INFO pages shortcuts...
  const QString man_proto = QStringLiteral("man:");
  const QString info_proto = QStringLiteral("info:");
  if( cmd[0] == '#' ||
      cmd.indexOf( man_proto ) == 0 ||
      cmd.indexOf( info_proto ) == 0 )
  {
    if( cmd.leftRef(2) == QLatin1String("##") )
      cmd = QStringLiteral("info:/") + cmd.mid(2);
    else if ( cmd[0] == '#' )
      cmd = QStringLiteral("man:/") + cmd.mid(1);

    else if ((cmd==info_proto) || (cmd==man_proto))
      cmd+='/';

    setFilteredUri( data, QUrl( cmd ));
    setUriType( data, KUriFilterData::Help );
    return true;
  }

  // Detect UNC style (aka windows SMB) URLs
  if ( cmd.startsWith( QLatin1String( "\\\\") ) )
  {
    // make sure path is unix style
    cmd.replace('\\', '/');
    cmd.prepend( QLatin1String( "smb:" ) );
    setFilteredUri( data, QUrl( cmd ));
    setUriType( data, KUriFilterData::NetProtocol );
    return true;
  }

  bool expanded = false;

  // Expanding shortcut to HOME URL...
  QString path;
  QString ref;
  QString query;
  QString nameFilter;

  if (QDir::isRelativePath(cmd) && QUrl(cmd).isRelative()) {
     path = cmd;
     qCDebug(category) << "path=cmd=" << path;
  } else {
    if (url.isLocalFile())
    {
      qCDebug(category) << "hasRef=" << url.hasFragment();
      // Split path from ref/query
      // but not for "/tmp/a#b", if "a#b" is an existing file,
      // or for "/tmp/a?b" (#58990)
      if( ( url.hasFragment() || !url.query().isEmpty() )
           && !url.path().endsWith(QLatin1Char('/')) ) // /tmp/?foo is a namefilter, not a query
      {
        path = url.path();
        ref = url.fragment();
        qCDebug(category) << "isLocalFile set path to" << path << "and ref to" << ref;
        query = url.query();
        if (path.isEmpty() && !url.host().isEmpty())
          path = '/';
      }
      else
      {
        if (cmd.startsWith(QLatin1String("file://"))) {
          path = cmd.mid(strlen("file://"));
        } else {
          path = cmd;
        }
        qCDebug(category) << "(2) path=cmd=" << path;
      }
    }
  }

  if( path[0] == '~' )
  {
    int slashPos = path.indexOf('/');
    if( slashPos == -1 )
      slashPos = path.length();
    if( slashPos == 1 )   // ~/
    {
      path.replace ( 0, 1, QDir::homePath() );
    }
    else // ~username/
    {
      const QString userName (path.mid( 1, slashPos-1 ));
      KUser user (userName);
      if( user.isValid() && !user.homeDir().isEmpty())
      {
        path.replace (0, slashPos, user.homeDir());
      }
      else
      {
        if (user.isValid()) {
          setErrorMsg(data, i18n("<qt><b>%1</b> does not have a home folder.</qt>", userName));
        } else {
          setErrorMsg(data, i18n("<qt>There is no user called <b>%1</b>.</qt>", userName));
        }
        setUriType( data, KUriFilterData::Error );
        // Always return true for error conditions so
        // that other filters will not be invoked !!
        return true;
      }
    }
    expanded = true;
  }
  else if ( path[0] == '$' ) {
    // Environment variable expansion.
    auto match = sEnvVarExp.match(path);
    if ( match.hasMatch() )
    {
      QByteArray exp = qgetenv( path.mid( 1, match.capturedLength() - 1 ).toLocal8Bit().data() );
      if (!exp.isEmpty()) {
        path.replace( 0, match.capturedLength(), QFile::decodeName(exp) );
        expanded = true;
      }
    }
  }

  if ( expanded || cmd.startsWith( '/' ) )
  {
    // Look for #ref again, after $ and ~ expansion (testcase: $QTDIR/doc/html/functions.html#s)
    // Can't use QUrl here, setPath would escape it...
    const int pos = path.indexOf('#');
    if ( pos > -1 )
    {
      const QString newPath = path.left( pos );
      if ( QFile::exists( newPath ) )
      {
        ref = path.mid( pos + 1 );
        path = newPath;
        qCDebug(category) << "Extracted ref: path=" << path << " ref=" << ref;
      }
    }
  }



  bool isLocalFullPath = QDir::isAbsolutePath(path);

  // Checking for local resource match...
  // Determine if "uri" is an absolute path to a local resource  OR
  // A local resource with a supplied absolute path in KUriFilterData
  const QString abs_path = data.absolutePath();

  const bool canBeAbsolute = (protocol.isEmpty() && !abs_path.isEmpty());
  const bool canBeLocalAbsolute = (canBeAbsolute && abs_path[0] =='/' && !isMalformed);
  bool exists = false;

  /*qCDebug(category) << "abs_path=" << abs_path
               << "protocol=" << protocol
               << "canBeAbsolute=" << canBeAbsolute
               << "canBeLocalAbsolute=" << canBeLocalAbsolute
               << "isLocalFullPath=" << isLocalFullPath;*/

  QT_STATBUF buff;
  if ( canBeLocalAbsolute )
  {
    QString abs = QDir::cleanPath( abs_path );
    // combine absolute path (abs_path) and relative path (cmd) into abs_path
    int len = path.length();
    if( (len==1 && path[0]=='.') || (len==2 && path[0]=='.' && path[1]=='.') )
        path += '/';
    qCDebug(category) << "adding " << abs << " and " << path;
    abs = QDir::cleanPath(abs + '/' + path);
    qCDebug(category) << "checking whether " << abs << " exists.";
    // Check if it exists
    if(QT_STAT(QFile::encodeName(abs), &buff) == 0) {
      path = abs; // yes -> store as the new cmd
      exists = true;
      isLocalFullPath = true;
    }
  }

  if (isLocalFullPath && !exists && !isMalformed) {
    exists = QT_STAT(QFile::encodeName(path), &buff) == 0;

    if ( !exists ) {
      // Support for name filter (/foo/*.txt), see also KonqMainWindow::detectNameFilter
      // If the app using this filter doesn't support it, well, it'll simply error out itself
      int lastSlash = path.lastIndexOf( '/' );
      if ( lastSlash > -1 && path.indexOf( ' ', lastSlash ) == -1 ) // no space after last slash, otherwise it's more likely command-line arguments
      {
        QString fileName = path.mid( lastSlash + 1 );
        QString testPath = path.left(lastSlash);
        if ((fileName.indexOf('*') != -1 || fileName.indexOf('[') != -1 || fileName.indexOf( '?' ) != -1)
                && QT_STAT(QFile::encodeName(testPath), &buff) == 0) {
          nameFilter = fileName;
          qCDebug(category) << "Setting nameFilter to" << nameFilter << "and path to" << testPath;
          path = testPath;
          exists = true;
        }
      }
    }
  }

  qCDebug(category) << "path =" << path << " isLocalFullPath=" << isLocalFullPath << " exists=" << exists << " url=" << url;
  if( exists )
  {
    QUrl u = QUrl::fromLocalFile(path);
    qCDebug(category) << "ref=" << ref << "query=" << query;
    u.setFragment(ref);
    u.setQuery(query);

    if (!KUrlAuthorized::authorizeUrlAction( QStringLiteral("open"), QUrl(), u))
    {
      // No authorization, we pretend it's a file will get
      // an access denied error later on.
      setFilteredUri( data, u );
      setUriType( data, KUriFilterData::LocalFile );
      return true;
    }

    // Can be abs path to file or directory, or to executable with args
    bool isDir = ((buff.st_mode & QT_STAT_MASK) == QT_STAT_DIR);
    if( !isDir && access ( QFile::encodeName(path).data(), X_OK) == 0 )
    {
      qCDebug(category) << "Abs path to EXECUTABLE";
      setFilteredUri( data, u );
      setUriType( data, KUriFilterData::Executable );
      return true;
    }

    // Open "uri" as file:/xxx if it is a non-executable local resource.
    if( isDir || (( buff.st_mode & QT_STAT_MASK ) == QT_STAT_REG) )
    {
      qCDebug(category) << "Abs path as local file or directory";
      if ( !nameFilter.isEmpty() )
        u.setPath(concatPaths(u.path(), nameFilter));
      setFilteredUri( data, u );
      setUriType( data, ( isDir ) ? KUriFilterData::LocalDir : KUriFilterData::LocalFile );
      return true;
    }

    // Should we return LOCAL_FILE for non-regular files too?
    qCDebug(category) << "File found, but not a regular file nor dir... socket?";
  }

  if( data.checkForExecutables())
  {
    // Let us deal with possible relative URLs to see
    // if it is executable under the user's $PATH variable.
    // We try hard to avoid parsing any possible command
    // line arguments or options that might have been supplied.
    QString exe = removeArgs( cmd );
    qCDebug(category) << "findExe with" << exe;

    if (!QStandardPaths::findExecutable( exe ).isNull() )
    {
      qCDebug(category) << "EXECUTABLE  exe=" << exe;
      setFilteredUri( data, QUrl::fromLocalFile( exe ));
      // check if we have command line arguments
      if( exe != cmd )
          setArguments(data, cmd.right(cmd.length() - exe.length()));
      setUriType( data, KUriFilterData::Executable );
      return true;
    }
  }

  // Process URLs of known and supported protocols so we don't have
  // to resort to the pattern matching scheme below which can possibly
  // slow things down...
  if ( !isMalformed && !isLocalFullPath && !protocol.isEmpty() )
  {
    qCDebug(category) << "looking for protocol" << protocol;
    if (isKnownProtocol(protocol))
    {
      setFilteredUri( data, url );
      if ( protocol == QLatin1String("man") || protocol == QLatin1String("help") )
        setUriType( data, KUriFilterData::Help );
      else
        setUriType( data, KUriFilterData::NetProtocol );
      return true;
    }
  }

  // Short url matches
  if ( !cmd.contains( ' ' ) )
  {
    // Okay this is the code that allows users to supply custom matches for
    // specific URLs using Qt's regexp class. This is hard-coded for now.
    // TODO: Make configurable at some point...
    Q_FOREACH(const URLHint& hint, m_urlHints)
    {
      qCDebug(category) << "testing regexp for" << hint.prepend;
      if (hint.regexp.indexIn(cmd) == 0)
      {
        const QString cmdStr = hint.prepend + cmd;
        QUrl url(cmdStr);
        qCDebug(category) << "match - prepending" << hint.prepend << "->" << cmdStr << "->" << url;
        setFilteredUri( data, url );
        setUriType( data, hint.type );
        return true;
      }
    }

    // No protocol and not malformed means a valid short URL such as kde.org or
    // user@192.168.0.1. However, it might also be valid only because it lacks
    // the scheme component, e.g. www.kde,org (illegal ',' before 'org'). The
    // check below properly deciphers the difference between the two and sends
    // back the proper result.
    if (protocol.isEmpty() && isPotentialShortURL(cmd))
    {
      QString urlStr = data.defaultUrlScheme();
      if (urlStr.isEmpty())
          urlStr = m_strDefaultUrlScheme;

      const int index = urlStr.indexOf(QLatin1Char(':'));
      if (index == -1 || !isKnownProtocol(urlStr.left(index)))
        urlStr += QStringLiteral("://");
      urlStr += cmd;

      QUrl url (urlStr);
      if (url.isValid())
      {
        setFilteredUri(data, url);
        setUriType(data, KUriFilterData::NetProtocol);
      }
      else if (isKnownProtocol(url.scheme()))
      {
        setFilteredUri(data, data.uri());
        setUriType(data, KUriFilterData::Error);
      }
      return true;
    }
  }

  // If we previously determined that the URL might be a file,
  // and if it doesn't exist... we'll pretend it exists.
  // This allows to use it for completion purposes.
  // (If you change this logic again, look at the commit that was testing
  //  for KUrlAuthorized::authorizeUrlAction("open"))
  if( isLocalFullPath && !exists )
  {
    QUrl u = QUrl::fromLocalFile(path);
    u.setFragment(ref);
    setFilteredUri(data, u);
    setUriType(data, KUriFilterData::LocalFile);
    return true;
  }

  // If we reach this point, we cannot filter this thing so simply return false
  // so that other filters, if present, can take a crack at it.
  return false;
}

KCModule* KShortUriFilter::configModule( QWidget*, const char* ) const
{
    return nullptr; //new KShortUriOptions( parent, name );
}

QString KShortUriFilter::configName() const
{
//    return i18n("&ShortURLs"); we don't have a configModule so no need for a configName that confuses translators
    return KUriFilterPlugin::configName();
}

void KShortUriFilter::configure()
{
  KConfig config( objectName() + QStringLiteral( "rc"), KConfig::NoGlobals );
  KConfigGroup cg( config.group("") );

  m_strDefaultUrlScheme = cg.readEntry( "DefaultProtocol", QStringLiteral("http://") );
  const EntryMap patterns = config.entryMap( QStringLiteral("Pattern") );
  const EntryMap protocols = config.entryMap( QStringLiteral("Protocol") );
  KConfigGroup typeGroup(&config, "Type");

  for( EntryMap::ConstIterator it = patterns.begin(); it != patterns.end(); ++it )
  {
    QString protocol = protocols[it.key()];
    if (!protocol.isEmpty())
    {
      int type = typeGroup.readEntry(it.key(), -1);
      if (type > -1 && type <= KUriFilterData::Unknown)
        m_urlHints.append( URLHint(it.value(), protocol, static_cast<KUriFilterData::UriTypes>(type) ) );
      else
        m_urlHints.append( URLHint(it.value(), protocol) );
    }
  }
}

K_PLUGIN_FACTORY_WITH_JSON(KShortUriFilterFactory, "kshorturifilter.json", registerPlugin<KShortUriFilter>();)

#include "kshorturifilter.moc"
