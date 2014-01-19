#include <QtCore/QTextStream>
#include <QtWidgets/QApplication>
#include <QtCore/QTimer>
#include <QtTest/QTest>

#include <kaboutdata.h>
#include <kwallet.h>
#include <QtDBus/QDBusConnectionInterface>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusReply>
#include <KLocalizedString>

static QTextStream _out( stdout, QIODevice::WriteOnly );

void openWallet()
{
	_out << "About to ask for wallet sync" << endl;

	KWallet::Wallet *w = KWallet::Wallet::openWallet( KWallet::Wallet::NetworkWallet(), 0, KWallet::Wallet::Synchronous );
    QVERIFY(w != 0);

	_out << "Got sync wallet: " << (w != 0) << endl;
}

int main( int argc, char *argv[] )
{
	QApplication app( argc, argv );
    app.setApplicationName("kwalletsync");
    app.setApplicationDisplayName(i18n("kwalletsync"));

	// force name with D-BUS
        QDBusReply<QDBusConnectionInterface::RegisterServiceReply> reply
            = QDBusConnection::sessionBus().interface()->registerService( "org.kde.kwalletsync",
                                                                QDBusConnectionInterface::ReplaceExistingService );

        if ( !reply.isValid() )
        {
                _out << "D-BUS name request returned " << reply.error().name() << endl;
        }

	openWallet();

	return 0;
}

// vim: set noet ts=4 sts=4 sw=4:

