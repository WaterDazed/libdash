/*
 * MediaObjectBuffer.cpp
 *****************************************************************************
 * Copyright (C) 2012, bitmovin Softwareentwicklung OG, All Rights Reserved
 *
 * Email: libdash-dev@vicky.bitmovin.net
 *
 * This source code and its use and distribution, is subject to the terms
 * and conditions of the applicable license agreement.
 *****************************************************************************/

#include "MediaObjectBuffer.h"
#include "../Input/DASHReceiver.h"
#include "../Input/DASHManager.h"
#include "../../../libdash/source/mpd/Segment.h"

using namespace libdash::framework::buffer;
using namespace libdash::framework::input;

using namespace dash::mpd;
using namespace dash::network;

MediaObjectBuffer::MediaObjectBuffer    (uint32_t maxcapacity) :
                   eos                  (false),
                   maxcapacity          (maxcapacity)
{
    InitializeConditionVariable (&this->full);
    InitializeConditionVariable (&this->empty);
    InitializeCriticalSection   (&this->monitorMutex);
}
MediaObjectBuffer::~MediaObjectBuffer   ()
{
    this->Clear();

    DeleteConditionVariable (&this->full);
    DeleteConditionVariable (&this->empty);
    DeleteCriticalSection   (&this->monitorMutex);
}

bool            MediaObjectBuffer::PushBack         (MediaObject *media)
{
    EnterCriticalSection(&this->monitorMutex);

    while(this->mediaobjects.size() >= this->maxcapacity && !this->eos)
        SleepConditionVariableCS(&this->empty, &this->monitorMutex, INFINITE);

    if(this->mediaobjects.size() >= this->maxcapacity)
    {
        LeaveCriticalSection(&this->monitorMutex);
        return false;
    }

    this->mediaobjects.push_back(media);

    WakeAllConditionVariable(&this->full);
    LeaveCriticalSection(&this->monitorMutex);
    this->Notify();
    return true;
}

bool            MediaObjectBuffer::PushBackWithCheck(MediaObject* media, DASHReceiver* dashReceiver) {
    EnterCriticalSection(&this->monitorMutex);

    while (this->mediaobjects.size() >= this->maxcapacity && !this->eos)
        SleepConditionVariableCS(&this->empty, &this->monitorMutex, INFINITE);
   
    std::string mediaRep = media->rep->GetId();
    std::string dashReceiverRep = dashReceiver->representation->GetId();
    //std::cout << mediaRep << "=?=" << dashReceiverRep << mediaRep.compare(dashReceiverRep) << std::endl;
    if (mediaRep.compare(dashReceiverRep) == 0) {
        if (this->mediaobjects.size() >= this->maxcapacity) {
            LeaveCriticalSection(&this->monitorMutex);
            return false;
        }
        this->mediaobjects.push_back(media);

        //Segment* tmp = dynamic_cast<Segment*>(media->segment);
        //std::cout << "add: " << tmp->AbsoluteURI() << std::endl;

        WakeAllConditionVariable(&this->full);
        this->Notify();
    }

    LeaveCriticalSection(&this->monitorMutex);
    return true;
}

MediaObject*    MediaObjectBuffer::Front            ()
{
    EnterCriticalSection(&this->monitorMutex);

    while(this->mediaobjects.size() == 0 && !this->eos)
        SleepConditionVariableCS(&this->full, &this->monitorMutex, INFINITE);

    if(this->mediaobjects.size() == 0)
    {
        LeaveCriticalSection(&this->monitorMutex);
        return NULL;
    }

    MediaObject *object = this->mediaobjects.front();

    LeaveCriticalSection(&this->monitorMutex);

    return object;
}
MediaObject* MediaObjectBuffer::FrontWithLock() {
    MediaObject* object = this->mediaobjects.front();

    //Segment* tmp = dynamic_cast<Segment*>(object->segment);
    //std::cout << "peek: " << tmp->AbsoluteURI() << std::endl;

    return object;
}
MediaObject*    MediaObjectBuffer::GetFront         (DASHManager* manager)
{
    EnterCriticalSection(&this->monitorMutex);

    while(this->mediaobjects.size() == 0 && !this->eos)
        SleepConditionVariableCS(&this->full, &this->monitorMutex, INFINITE);

    if(this->mediaobjects.size() == 0)
    {
        LeaveCriticalSection(&this->monitorMutex);
        return NULL;
    }

    if (manager)
        EnterCriticalSection(&manager->receiver->monitorMutex);

    MediaObject *object = this->mediaobjects.front();
    this->mediaobjects.pop_front();
    manager->receiver->latestDecodedsegmentNumber = (int)(object->segmentNumber);

    //Segment* tmp = dynamic_cast<Segment*>(object->segment);
    //std::cout << "get: " << tmp->AbsoluteURI() << std::endl;

    WakeAllConditionVariable(&this->empty);

    if (manager)
        LeaveCriticalSection(&manager->receiver->monitorMutex);
    LeaveCriticalSection(&this->monitorMutex);
    this->Notify();

    return object;
}
uint32_t        MediaObjectBuffer::Length           ()
{
    EnterCriticalSection(&this->monitorMutex);

    uint32_t ret = this->mediaobjects.size();

    LeaveCriticalSection(&this->monitorMutex);

    return ret;
}

uint32_t        MediaObjectBuffer::LengthWithLock() {
    uint32_t ret = this->mediaobjects.size();
    return ret;
}

void            MediaObjectBuffer::PopFront         ()
{
    EnterCriticalSection(&this->monitorMutex);

    this->mediaobjects.pop_front();

    WakeAllConditionVariable(&this->empty);
    LeaveCriticalSection(&this->monitorMutex);
    this->Notify();
}
void            MediaObjectBuffer::PopFrontWithLock() 
{
    MediaObject* object = this->mediaobjects.front();
    this->mediaobjects.pop_front();

    //Segment* tmp = dynamic_cast<Segment*>(object->segment);
    //std::cout << "throw: " << tmp->AbsoluteURI() << std::endl;

    delete object;

    WakeAllConditionVariable(&this->empty);
    this->Notify();
}
void            MediaObjectBuffer::SetEOS           (bool value)
{
    EnterCriticalSection(&this->monitorMutex);

    for (size_t i = 0; i < this->mediaobjects.size(); i++)
        this->mediaobjects.at(i)->AbortDownload();

    this->eos = value;

    WakeAllConditionVariable(&this->empty);
    WakeAllConditionVariable(&this->full);
    LeaveCriticalSection(&this->monitorMutex);
}
void            MediaObjectBuffer::AttachObserver   (IMediaObjectBufferObserver *observer)
{
    this->observer.push_back(observer);
}
void            MediaObjectBuffer::Notify           ()
{
    for(size_t i = 0; i < this->observer.size(); i++)
        this->observer.at(i)->OnBufferStateChanged((int)((double)this->mediaobjects.size()/(double)this->maxcapacity*100.0));
}
void            MediaObjectBuffer::ClearTail        ()
{
    EnterCriticalSection(&this->monitorMutex);

    int size = this->mediaobjects.size() - 1;

    if (size < 1)
    {
        LeaveCriticalSection(&this->monitorMutex);
        return;
    }

    MediaObject* object = this->mediaobjects.front();
    this->mediaobjects.pop_front();
    for(int i=0; i < size; i++)
    {
        delete this->mediaobjects.front();
        this->mediaobjects.pop_front();
    }

    this->mediaobjects.push_back(object);
    WakeAllConditionVariable(&this->empty);
    WakeAllConditionVariable(&this->full);
    LeaveCriticalSection(&this->monitorMutex);
    this->Notify();
}
void            MediaObjectBuffer::Clear            ()
{
    EnterCriticalSection(&this->monitorMutex);

    for(int i=0; i < this->mediaobjects.size(); i++)
    {
        delete this->mediaobjects.front();
        this->mediaobjects.pop_front();
    }

    WakeAllConditionVariable(&this->empty);
    WakeAllConditionVariable(&this->full);
    LeaveCriticalSection(&this->monitorMutex);
    this->Notify();
}
uint32_t        MediaObjectBuffer::Capacity         ()
{
    return this->maxcapacity;
}