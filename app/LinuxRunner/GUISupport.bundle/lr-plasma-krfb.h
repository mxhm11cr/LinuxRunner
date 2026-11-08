// KDE KRFB 26.08.2 plugin ABI declarations used by LinuxRunner's local bridge.
// Original copyright and GPL notices follow; see plasma-bridge-notices.txt.
#pragma once
#include <QtCore/qglobal.h>
#define KRFBPRIVATE_EXPORT Q_DECL_IMPORT

/**
 * This file should always be included instead of <rfb/rfb.h> because otherwise it's redefinition
 * of TRUE and FALSE plays havoc with other things.
 */

#ifndef KRFB_RFB_H
#define KRFB_RFB_H

#include "rfb/rfb.h"

#ifdef max
#undef max
#endif

#undef TRUE
#undef FALSE

#endif // Header guard

/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2007 Alessandro Praduroux <pradu@pradu.it>

   SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H





#include <QList>
#include <QObject>
#include <QRect>
#include <QVariant>
#include <QWidget>

class FrameBuffer;
/**
    @author Alessandro Praduroux <pradu@pradu.it>
*/
class KRFBPRIVATE_EXPORT FrameBuffer : public QObject
{
    Q_OBJECT

public:
    explicit FrameBuffer(QObject *parent = nullptr);

    ~FrameBuffer() override;

    char *data();

    virtual QList<QRect> modifiedTiles();
    virtual int paddedWidth();
    virtual int width();
    virtual int height();
    virtual int depth();
    virtual void startMonitor();
    virtual void stopMonitor();
    virtual QPoint cursorPosition();

    virtual void getServerFormat(rfbPixelFormat &format);

    virtual QVariant customProperty(const QString &property) const;

Q_SIGNALS:
    void frameBufferChanged();

protected:
    char *fb = nullptr;
    QList<QRect> tiles;

private:
    Q_DISABLE_COPY(FrameBuffer)
};

#endif

/*
   This file is part of the KDE project

   SPDX-FileCopyrightText: 2010 Collabora Ltd.
   SPDX-FileContributor: George Kiagiadakis <george.kiagiadakis@collabora.co.uk>
   SPDX-FileCopyrightText: 2007 Alessandro Praduroux <pradu@pradu.it>
   SPDX-FileCopyrightText: 2001-2003 Tim Jansen <tim@tjansen.de>

   SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef EVENTS_H
#define EVENTS_H





#include <QObject>

class KRFBPRIVATE_EXPORT EventHandler : public QObject
{
    Q_OBJECT

public:
    explicit EventHandler(QObject *parent = nullptr);
    ~EventHandler() override = default;
    virtual void handleKeyboard(bool down, rfbKeySym key) = 0;
    virtual void handlePointer(int buttonMask, int x, int y) = 0;

    void setFrameBufferPlugin(const QSharedPointer<FrameBuffer> &frameBuffer);
    QSharedPointer<FrameBuffer> frameBuffer();

private:
    // Used to track framebuffer plugin which we need for xdp event plugin
    QSharedPointer<FrameBuffer> fb;
};

#endif

/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2009 Collabora Ltd <info@collabora.co.uk>
   SPDX-FileContributor: George Goldberg <george.goldberg@collabora.co.uk>

   SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef LIB_KRFB_FRAMEBUFFERPLUGIN_H
#define LIB_KRFB_FRAMEBUFFERPLUGIN_H



#include <QVariantList>
#include <QWidget>

class FrameBuffer;

class KRFBPRIVATE_EXPORT FrameBufferPlugin : public QObject
{
    Q_OBJECT

public:
    explicit FrameBufferPlugin(QObject *parent, const QVariantList &args);
    ~FrameBufferPlugin() override;

    virtual FrameBuffer *frameBuffer(const QVariantMap &args) = 0;
};

#endif // Header guard
