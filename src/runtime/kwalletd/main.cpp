/**
  * This file is part of the KDE project
  * Copyright (C) 2008 Michael Leupold <lemma@confuego.org>
  * Copyright (C) 2014 Alex Fiestas <afiestas@kde.org>
  *
  * This library is free software; you can redistribute it and/or
  * modify it under the terms of the GNU Library General Public
  * License version 2 as published by the Free Software Foundation.
  *
  * This library is distributed in the hope that it will be useful,
  * but WITHOUT ANY WARRANTY; without even the implied warranty of
  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  * Library General Public License for more details.
  *
  * You should have received a copy of the GNU Library General Public License
  * along with this library; see the file COPYING.LIB.  If not, write to
  * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  * Boston, MA 02110-1301, USA.
  */

#include <QApplication>
#include <QDebug>
#include <QtCore/QString>
#include <QSessionManager>
#include <KLocalizedString>
#include <KAboutData>
#include <KConfig>
#include <KConfigGroup>
#include <KDBusService>

#include <stdio.h>

#include "backend/kwalletbackend.h" //For the hash size
#include "kwalletd.h"
#include "kwalletd_version.h"
#include "migrationagent.h"

#ifndef Q_OS_WIN
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <sys/stat.h>
#include <gcrypt.h>

#define BSIZE 1000
static int pipefd = 0;
static int socketfd = 0;
#endif

static bool isWalletEnabled()
{
    KConfig cfg(QStringLiteral("kwalletrc"));
    KConfigGroup walletGroup(&cfg, "Wallet");
    return walletGroup.readEntry("Enabled", true);
}

#ifndef Q_OS_WIN
//Waits until the PAM_MODULE sends the hash
static char *waitForHash()
{
    printf("kwalletd5: Waiting for hash on %d-\n", pipefd);
    int totalRead = 0;
    int readBytes = 0;
    int attempts = 0;
    char *buf = (char*)malloc(sizeof(char) * PBKDF2_SHA512_KEYSIZE);
    memset(buf, '\0', PBKDF2_SHA512_KEYSIZE);
    while(totalRead != PBKDF2_SHA512_KEYSIZE) {
        readBytes = read(pipefd, buf + totalRead, PBKDF2_SHA512_KEYSIZE - totalRead);
        if (readBytes == -1 || attempts > 5) {
            free(buf);
            return NULL;
        }
        totalRead += readBytes;
        ++attempts;
    }

    close(pipefd);
    return buf;
}

//Waits until startkde sends the environment variables
static int waitForEnvironment()
{
    printf("kwalletd5: waitingForEnvironment on: %d\n", socketfd);

    int s2;
    struct sockaddr_un remote;
    socklen_t t = sizeof(remote);
    if ((s2 = accept(socketfd, (struct sockaddr *)&remote, &t)) == -1) {
        fprintf(stdout, "kwalletd5: Couldn't accept incoming connection\n");
        return -1;
    }
    printf("kwalletd5: client connected\n");

    char str[BSIZE] = {'\0'};

    int chop = 0;
    FILE *s3 = fdopen(dup(s2), "r");
    while(!feof(s3)) {
        if (fgets(str, BSIZE, s3)) {
            chop = strlen(str) - 1;
            if (str[chop] == '\n') {
                str[chop] = '\0';
            }
            putenv(strdup(str));
        }
    }
    fclose(s3);

    printf("kwalletd5: client disconnected\n");
    close(socketfd);
    return 1;
}

char* checkPamModule(int argc, char **argv)
{
    printf("kwalletd5: Checking for pam module\n");
    char *hash = NULL;
    int x = 1;
    for (; x < argc; ++x) {
        if (strcmp(argv[x], "--pam-login") != 0) {
            continue;
        }
        printf("kwalletd5: Got pam-login param\n");
        argv[x] = NULL;
        x++;
        //We need at least 2 extra arguments after --pam-login
        if (x + 1 > argc) {
            printf("kwalletd5: Invalid arguments (less than needed)\n");
            return NULL;
        }

        //first socket for the hash, comes from a pipe
        pipefd = atoi(argv[x]);
        argv[x] = NULL;
        x++;
        //second socket for environment, comes from a localsocket
        socketfd = atoi(argv[x]);
        argv[x] = NULL;
        break;
    }

    if (!pipefd || !socketfd) {
        printf("Lacking a socket, pipe: %d, env:%d\n", pipefd, socketfd);
        return NULL;
    }

    hash = waitForHash();

    if (hash == NULL || waitForEnvironment() == -1) {
        printf("kwalletd5: Hash or environment not received\n");
        free(hash);
        return NULL;
    }

    return hash;
}
#endif

#define KWALLET_SALTSIZE 56
#define KWALLET_KEYSIZE 56
#define KWALLET_ITERATIONS 50000

static char* createNewSalt(const char *path)
{
    char *salt = (char *)gcry_random_bytes(KWALLET_SALTSIZE, GCRY_STRONG_RANDOM);
    FILE *fd = fopen(path, "w");

    //If the file can't be created
    if (fd == NULL)
        return NULL;

    fwrite(salt, KWALLET_SALTSIZE, 1, fd);
    fclose(fd);

    return salt;
}

int kwallet_empty_hash(const char *saltpath, char *key)
{
    struct stat info;
    char *salt = NULL;
    if (stat(saltpath, &info) != 0 || info.st_size == 0) {
        salt = createNewSalt(saltpath);
    } else {
        FILE *fd = fopen(saltpath, "r");
        if (fd == NULL)
            return 1;

        salt = (char*) malloc(sizeof(char) * KWALLET_SALTSIZE);
        memset(salt, '\0', KWALLET_SALTSIZE);
        fread(salt, KWALLET_SALTSIZE, 1, fd);
        fclose(fd);
    }

    if (salt == NULL)
        return 1;

    gcry_error_t error;
    error = gcry_control(GCRYCTL_INIT_SECMEM, 32768, 0);
    if (error != 0)
        return 1;

    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    const char *passphrase="";

    error = gcry_kdf_derive(passphrase, strlen(passphrase),
                            GCRY_KDF_PBKDF2, GCRY_MD_SHA512,
                            salt, KWALLET_SALTSIZE,
                            KWALLET_ITERATIONS, KWALLET_KEYSIZE, key);
    return 0;
}

#ifdef HAVE_KF5INIT
extern "C" Q_DECL_EXPORT int kdemain(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    char *hash = NULL;
#ifndef Q_OS_WIN
    if (getenv("PAM_KWALLET5_LOGIN")) {
        hash = checkPamModule(argc, argv);
    } else {
        QString localKwalletPath = QDir::homePath() + "/.local/share/kwalletd";
        if (!QFile::exists(localKwalletPath + "/kdewallet.salt")) {
            if (!QDir().exists(localKwalletPath)) {
                QDir().mkpath(localKwalletPath);
            }
            char *key = (char *)malloc(sizeof(char) * KWALLET_KEYSIZE);
            if (kwallet_empty_hash(QString(localKwalletPath + "/kdewallet.salt").toStdString().c_str(), key) == 0) {
                hash = key;
            }
        }
    }
#endif

    QApplication app(argc, argv);
    // this kwalletd5 program should be able to start with KDE4's kwalletd
    // using kwalletd name would prevent KDBusService unique instance to initialize
    // so we setApplicationName("kwalletd5")
    app.setApplicationName(QStringLiteral("kwalletd5"));
    app.setApplicationDisplayName(i18n("KDE Wallet Service"));
    app.setOrganizationDomain(QStringLiteral("kde.org"));
    app.setApplicationVersion(KWALLETD_VERSION_STRING);

    KAboutData aboutdata(I18N_NOOP("kwalletd"),
                         i18n("KDE Wallet Service"),
                         KWALLETD_VERSION_STRING,
                         i18n("KDE Wallet Service"),
                         KAboutLicense::LGPL,
                         i18n("(C) 2002-2013, The KDE Developers"));
    aboutdata.addAuthor(i18n("Valentin Rusu"), i18n("Maintainer, GPG backend support"), QStringLiteral("kde@rusu.info"));
    aboutdata.addAuthor(i18n("Michael Leupold"), i18n("Former Maintainer"), QStringLiteral("lemma@confuego.org"));
    aboutdata.addAuthor(i18n("George Staikos"), i18n("Former maintainer"), QStringLiteral("staikos@kde.org"));
    aboutdata.addAuthor(i18n("Thiago Maceira"), i18n("D-Bus Interface"), QStringLiteral("thiago@kde.org"));

    KWalletD walletd;
    MigrationAgent migrationAgent(&walletd, hash);
    KDBusService dbusUniqueInstance(KDBusService::Unique);

    // NOTE: the command should be parsed only after KDBusService instantiation
    QCommandLineParser cmdParser;
    aboutdata.setupCommandLine(&cmdParser);
    cmdParser.process(app);

    aboutdata.setProgramIconName(QStringLiteral("kwalletmanager"));

    app.setQuitOnLastWindowClosed(false);
    auto disableSessionManagement = [](QSessionManager &sm) {
        sm.setRestartHint(QSessionManager::RestartNever);
    };
    QObject::connect(&app, &QGuiApplication::commitDataRequest, disableSessionManagement);
    QObject::connect(&app, &QGuiApplication::saveStateRequest, disableSessionManagement);

    // check if kwallet is disabled
    if (!isWalletEnabled()) {
        qDebug() << "kwalletd is disabled!";
        return (0);
    }

    qDebug() << "kwalletd5 started";

#ifndef Q_OS_WIN
    if (hash) {
        QByteArray passHash(hash, PBKDF2_SHA512_KEYSIZE);
        int wallet = walletd.pamOpen(KWallet::Wallet::LocalWallet(), passHash, 0);
        if (wallet < 0) {
            qWarning() << "Wallet failed to get opened by PAM, error code is" << wallet;
        } else {
            qDebug() << "Wallet opened by PAM";
        }
        free(hash);
    }
#endif

    return app.exec();
}
