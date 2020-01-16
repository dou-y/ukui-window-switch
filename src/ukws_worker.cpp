/*
 * Copyright (C) 2020 Tianjin KYLIN Information Technology Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 */

#include "ukws_worker.h"

#include <sched.h>
#include <errno.h>

#include <QDebug>

UkwsWorker::UkwsWorker(QObject *parent) : QObject(parent)
{
    init();
    doingThread = nullptr;
}

void UkwsWorker::init()
{
    status = UkwsWorker::Stopped;
    cpu = 0;
    workList.clear();
}

void UkwsWorker::doWork()
{
    cpu_set_t mask;

    status = UkwsWorker::Running;

    // 设置CPU亲和力
    CPU_ZERO(&mask);    /* 初始化set集，将set置为空*/
    CPU_SET(cpu, &mask);
    if (sched_setaffinity(0, sizeof(mask), &mask) == -1) {
        qWarning("Worker %d set CPU affinity failue, ERROR:%s", cpu, strerror(errno));
    }

    // 开始处理
    UkwsWindowBox *wb;
    foreach (wb, workList) {
        if (status == UkwsWorker::Running)
            wb->setThumbnailByWnck();
        else
            break;
    }

    // 设置状态
    status = UkwsWorker::Stopped;
    emit workDone();
}

void UkwsWorker::stopWork()
{
    status = UkwsWorker::Stopping;
}

bool UkwsWorker::isStopped()
{
    if (status == UkwsWorker::Stopped)
        return true;
    else
        return false;
}
