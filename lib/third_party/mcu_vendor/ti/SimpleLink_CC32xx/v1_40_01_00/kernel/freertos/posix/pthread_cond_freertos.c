/*
 * Copyright (c) 2016-2017, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 *  ======== pthread_cond_freertos.c ========
 */

#include <stdint.h>

#include <FreeRTOS.h>
#include <portmacro.h>
#include <semphr.h>
#include <task.h>

#include "pthread.h"
#include "time.h"
#include "errno.h"

/*
 *  ======== ListElem ========
 */
typedef struct ListElem {
    struct ListElem *next;
    struct ListElem *prev;
} ListElem, List;

/*
 *  ======== CondElem ========
 */
typedef struct CondElem {
    ListElem          qElem;
    SemaphoreHandle_t sem;
} CondElem;

/*
 *  ======== pthread_cond_Obj ========
 */
typedef struct pthread_cond_Obj {
    List         waitList;
} pthread_cond_Obj;


/*
 *  Function for obtaining a timeout in Clock ticks from the difference of
 *  an absolute time and the current time.
 */
extern int _clock_abstime2ticks(clockid_t clockId,
        const struct timespec *abstime, uint32_t *ticks);

static int condWait(pthread_cond_Obj *cond, pthread_mutex_t *mutex,
        uint32_t timeout);

/*
 *************************************************************************
 *                      pthread_condattr
 *************************************************************************
 */
/*
 *  ======== pthread_condattr_destroy ========
 */
int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    return (0);
}

/*
 *  ======== pthread_condattr_init ========
 */
int pthread_condattr_init(pthread_condattr_t * attr)
{
    return (0);
}

/*
 *************************************************************************
 *                      pthread_cond
 *************************************************************************
 */
/*
 *  ======== pthread_cond_broadcast ========
 */
int pthread_cond_broadcast(pthread_cond_t *cond)
{
    pthread_cond_Obj *condObj = *((pthread_cond_Obj **)cond);
    CondElem     *condElem;

    /* Disable the scheduler */
    vTaskSuspendAll();

    while (condObj->waitList.next != NULL) {
        /* Remove from the queue */
        condElem = (CondElem *)(condObj->waitList.next);
        condObj->waitList.next = condElem->qElem.next;

        /* Unblock the thread waiting on the condition variable. */
        xSemaphoreGive(condElem->sem);
    }

    /* Re-enable the scheduler */
    xTaskResumeAll();

    return (0);
}

/*
 *  ======== pthread_cond_destroy ========
 */
int pthread_cond_destroy(pthread_cond_t *cond)
{

    return (0);
}

/*
 *  ======== pthread_cond_init ========
 */
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    pthread_cond_Obj *condObj = NULL;

    *cond = NULL;

    condObj = pvPortMalloc(sizeof(pthread_cond_Obj));
    if (condObj == NULL) {
        return (ENOMEM);
    }

    condObj->waitList.next = condObj->waitList.prev = NULL;

    *cond = (pthread_cond_t)condObj;
    return (0);
}

/*
 *  ======== pthread_cond_signal ========
 */
int pthread_cond_signal(pthread_cond_t *cond)
{
    pthread_cond_Obj *condObj = *((pthread_cond_Obj **)cond);
    CondElem         *elem;

    /*
     *  The calling thread is not required to hold the mutex when
     *  calling pthread_cond_broadcast() or pthread_cond_signal().
     */

    /* Disable the scheduler */
    vTaskSuspendAll();

    if (condObj->waitList.next != NULL) {
        /* Remove from the queue */
        elem = (CondElem *)(condObj->waitList.next);
        condObj->waitList.next = elem->qElem.next;

        /* Unblock the thread waiting on the condition variable. */
        xSemaphoreGive(elem->sem);
    }

    /* Re-enable the scheduler */
    xTaskResumeAll();

    return (0);
}

/*
 *  ======== pthread_cond_timedwait ========
 */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
        const struct timespec *abstime)
{
    pthread_cond_Obj  *condObj = *((pthread_cond_Obj **)cond);
    uint32_t           timeout;

    if (_clock_abstime2ticks(CLOCK_MONOTONIC, abstime, &timeout) != 0) {
        return (EINVAL);
    }

    return (condWait(condObj, mutex, timeout));
}

/*
 *  ======== pthread_cond_wait ========
 */
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    pthread_cond_Obj *condObj = *((pthread_cond_Obj **)cond);

    return (condWait(condObj, mutex, portMAX_DELAY));
}

/*
 *  ======== condWait ========
 */
static int condWait(pthread_cond_Obj *cond, pthread_mutex_t *mutex,
        uint32_t timeout)
{
    CondElem    condElem;
    int         ret = 0;

    /*
     *  TODO:  Would like this to be static.  This requires that application
     *  implement hook functions:
     *      vApplicationGetIdleTaskMemory()
     *      vApplicationGetTimerTaskMemory()
     */
    condElem.sem = xSemaphoreCreateBinary();

    if (condElem.sem == NULL) {
        return (ENOMEM);
    }

    /*
     *  The calling thread is holding mutex but threads signalling
     *  the condition variable are not required to hold the mutex.
     *  Therefore, we need to disable the scheduler to protect the
     *  condition variable's waitList.
     */

    /* Disable the scheduler */
    vTaskSuspendAll();

    condElem.qElem.next = cond->waitList.next;
    condElem.qElem.prev = &(cond->waitList);

    if (cond->waitList.next) {
        cond->waitList.next->prev = (ListElem *)&condElem;
    }
    cond->waitList.next = (ListElem *)&condElem;

    /* Re-enable the scheduler */
    xTaskResumeAll();

    pthread_mutex_unlock(mutex);

    if (xSemaphoreTake(condElem.sem, (TickType_t)timeout) != pdTRUE) {
        /* Disable the scheduler */
        vTaskSuspendAll();

        if (condElem.qElem.next) {
            condElem.qElem.next->prev = condElem.qElem.prev;
        }
        condElem.qElem.prev->next = condElem.qElem.next;

        /* Re-enable the scheduler */
        xTaskResumeAll();

        ret = ETIMEDOUT;
    }

    vSemaphoreDelete(condElem.sem);

    pthread_mutex_lock(mutex);

    return (ret);
}
