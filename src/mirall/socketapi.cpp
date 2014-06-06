/*
 * Copyright (C) by Dominik Schmidt <dev@dominik-schmidt.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */


#include "socketapi.h"

#include "mirall/mirallconfigfile.h"
#include "mirall/folderman.h"
#include "mirall/folder.h"
#include "mirall/utility.h"
#include "mirall/theme.h"

#include <QDebug>
#include <QUrl>
#include <QLocalSocket>
#include <QLocalServer>
#include <QMetaObject>
#include <QStringList>
#include <QScopedPointer>
#include <QFile>
#include <QDir>
#include <QApplication>

namespace Mirall {

#define DEBUG qDebug() << "SocketApi: "

SocketApi::SocketApi(QObject* parent, const QUrl& localFile)
    : QObject(parent)
    , _localServer(0)
{
    QString socketPath;
    if (Utility::isWindows()) {
        socketPath = QLatin1String("\\\\.\\pipe\\")
                + Theme::instance()->appName();
    } else {
        socketPath = localFile.toLocalFile();

    }

    // setup socket
    _localServer = new QLocalServer(this);
    QLocalServer::removeServer(socketPath);
    if(!_localServer->listen(socketPath)) {
        DEBUG << "can't start server" << socketPath;
    } else {
        DEBUG << "server started, listening at " << socketPath;
    }
    connect(_localServer, SIGNAL(newConnection()), this, SLOT(slotNewConnection()));

    // folder watcher
    connect(FolderMan::instance(), SIGNAL(folderSyncStateChange(QString)), SLOT(slotSyncStateChanged(QString)));
}

SocketApi::~SocketApi()
{
    DEBUG << "dtor";
    _localServer->close();
}

void SocketApi::slotNewConnection()
{
    QLocalSocket* socket = _localServer->nextPendingConnection();
    if( ! socket ) {
        return;
    }
    DEBUG << "New connection " << socket;
    connect(socket, SIGNAL(readyRead()), this, SLOT(slotReadSocket()));
    connect(socket, SIGNAL(disconnected()), this, SLOT(onLostConnection()));
    Q_ASSERT(socket->readAll().isEmpty());

    _listeners.append(socket);
}

void SocketApi::onLostConnection()
{
    DEBUG << "Lost connection " << sender();

    QLocalSocket* socket = qobject_cast< QLocalSocket* >(sender());
    _listeners.removeAll(socket);
}


void SocketApi::slotReadSocket()
{
    QLocalSocket* socket = qobject_cast<QLocalSocket*>(sender());
    Q_ASSERT(socket);

    while(socket->canReadLine()) {
        QString line = QString::fromUtf8(socket->readLine()).trimmed();
        QString command = line.split(":").first();
        QString function = QString(QLatin1String("command_")).append(command);

        QString functionWithArguments = function + QLatin1String("(QString,QLocalSocket*)");
        int indexOfMethod = this->metaObject()->indexOfMethod(functionWithArguments.toAscii());

        QString argument = line.remove(0, command.length()+1).trimmed();
        if(indexOfMethod != -1) {
            QMetaObject::invokeMethod(this, function.toAscii(), Q_ARG(QString, argument), Q_ARG(QLocalSocket*, socket));
        } else {
            DEBUG << "The command is not supported by this version of the client:" << command << "with argument:" << argument;
        }
    }
}

void SocketApi::slotSyncStateChanged(const QString&)
{
    broadcastMessage("UPDATE_VIEW");
}


void SocketApi::sendMessage(QLocalSocket* socket, const QString& message)
{
    DEBUG << "Sending message: " << message;
    QString localMessage = message;
    socket->write(localMessage.append("\n").toUtf8());
}

void SocketApi::broadcastMessage(const QString& message)
{
    DEBUG << "Broadcasting to" << _listeners.count() << "listeners: " << message;
    foreach(QLocalSocket* current, _listeners)
    {
        sendMessage(current, message);
    }
}

void SocketApi::command_RETRIEVE_FOLDER_STATUS(const QString& argument, QLocalSocket* socket)
{
    qDebug() << Q_FUNC_INFO << argument;
    QString statusString;

    if( !socket ) {
        qDebug() << "No valid socket object.";
        return;
    }

    Folder* folder = FolderMan::instance()->folderForPath( argument );
    // this can happen in offline mode e.g.: nothing to worry about
    if (!folder) {
        DEBUG << "folder offline or not watched:" << argument;
        statusString = QLatin1String("NOP");
    }

    QDir dir(argument);
    if( statusString.isEmpty() ) {
        const QStringList fileEntries = dir.entryList( QDir::Files );
        foreach(const QString file, fileEntries) {
            const QString absoluteFilePath = dir.absoluteFilePath(file);
            SyncFileStatus fileStatus = folder->fileStatus( absoluteFilePath.mid(folder->path().length()) );
            if( fileStatus == FILE_STATUS_STAT_ERROR ) {
                qDebug() << "XXXXXXXXXXXX FileStatus is STAT ERROR for " << absoluteFilePath;
            }
            if( fileStatus != FILE_STATUS_SYNC ) {
                qDebug() << "SyncFileStatus for " << absoluteFilePath << " is " << fileStatus;
                // we found something that is not in sync
                statusString = QLatin1String("NEED_SYNC");
                break;
            }
        }
    }

    if( statusString.isEmpty() ) { // if  it is still empty, we check the dirs recursively.
        const QStringList dirEntries = dir.entryList( QDir::AllDirs | QDir::NoDotAndDotDot );

        foreach(const QString entry, dirEntries) {
            QString absoluteFilePath = dir.absoluteFilePath(entry);
            SyncFileStatus sfs = folder->recursiveFolderStatus( absoluteFilePath.mid(folder->path().length()) );
            if( sfs != FILE_STATUS_SYNC ) {
                statusString = QLatin1String("NEED_SYNC");
                break;
            }
        }
    }

    if( statusString.isEmpty() ) {
        statusString = QLatin1String("OK");
    }

    QString message = QLatin1String("STATUS:")+statusString+QLatin1Char(':')+argument;
    sendMessage(socket, message);
}

void SocketApi::command_RETRIEVE_FILE_STATUS(const QString& argument, QLocalSocket* socket)
{
    if( !socket ) {
        qDebug() << "No valid socket object.";
        return;
    }

    qDebug() << Q_FUNC_INFO << argument;

    QString statusString;

    Folder* folder = FolderMan::instance()->folderForPath( argument );
    // this can happen in offline mode e.g.: nothing to worry about
    if (!folder) {
        DEBUG << "folder offline or not watched:" << argument;
        statusString = QLatin1String("NOP");
    }

    if( statusString.isEmpty() ) {
        SyncFileStatus fileStatus = folder->fileStatus( argument.mid(folder->path().length()) );
        if( fileStatus == FILE_STATUS_STAT_ERROR ) {
            qDebug() << "XXXXXXXXXXXX FileStatus is STAT ERROR for " << argument;
        }
        if( fileStatus != FILE_STATUS_SYNC ) {
            qDebug() << "SyncFileStatus for " << argument << " is " << fileStatus;
            // we found something that is not in sync
            statusString = QLatin1String("NEED_SYNC");
        }
    }

    if( statusString.isEmpty() ) {
        statusString = QLatin1String("OK");
    }

    QString message = QLatin1String("STATUS:")+statusString+QLatin1Char(':')+argument;
    sendMessage(socket, message);
}

} // namespace Mirall
