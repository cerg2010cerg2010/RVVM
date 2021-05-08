/*
threading.c - Threads
Copyright (C) 2021  LekKit <github.com/LekKit>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "threading.h"
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

thread_handle_t thread_create(thread_func_t func_name, void *arg)
{
    thread_handle_t handle;
#ifdef _WIN32
    handle = malloc(sizeof(HANDLE));
    if (handle) *(HANDLE*)handle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)(const void*)func_name, arg, 0, NULL);
#else
    handle = malloc(sizeof(pthread_t));
    if (handle) pthread_create((pthread_t*)handle, NULL, func_name, arg);
#endif
    return handle;
}

void* thread_join(thread_handle_t handle)
{
    if (handle == NULL) return NULL;
    void* ret;
#ifdef _WIN32
    DWORD ltmp;
    WaitForSingleObject(*(HANDLE*)handle, INFINITE);
    GetExitCodeThread(*(HANDLE*)handle, &ltmp);
    ret = (void*)ltmp;
#else
    pthread_join(*(pthread_t*)handle, &ret);
#endif
    free(handle);
    return ret;
}

void thread_kill(thread_handle_t handle)
{
    if (handle == NULL) return;
#ifdef _WIN32
    TerminateThread(*(HANDLE*)handle, 0);
#else
    pthread_cancel(*(pthread_t*)handle);
#endif
    free(handle);
}
