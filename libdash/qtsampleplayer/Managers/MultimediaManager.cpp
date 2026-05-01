/*
 * MultimediaManager.cpp
 *****************************************************************************
 * Copyright (C) 2013, bitmovin Softwareentwicklung OG, All Rights Reserved
 *
 * Email: libdash-dev@vicky.bitmovin.net
 *
 * This source code and its use and distribution, is subject to the terms
 * and conditions of the applicable license agreement.
 *****************************************************************************/

#include "MultimediaManager.h"
#include "../libdashframework/MPD/AdaptationSetHelper.h"

using namespace libdash::framework::adaptation;
using namespace libdash::framework::buffer;
using namespace sampleplayer::managers;
using namespace sampleplayer::renderer;
using namespace dash::mpd;

#define SEGMENTBUFFER_SIZE 2

MultimediaManager::MultimediaManager(QTGLRenderer* videoElement, QTAudioRenderer* audioElement) :
	videoElement(videoElement),
	audioElement(audioElement),
	mpd(NULL),
	videoLogic(NULL),
	videoStream(NULL),
	audioLogic(NULL),
	audioStream(NULL),
	isStarted(false),
	framesDisplayed(0),
	segmentsDownloaded(0),
	isVideoRendering(false),
	isAudioRendering(false),
	speed(1){
	InitializeCriticalSection(&this->monitorMutex);

	this->manager = CreateDashManager();
	av_register_all();
}
MultimediaManager::~MultimediaManager() {
	this->Stop();
	this->manager->Delete();

	DeleteCriticalSection(&this->monitorMutex);
}

IMPD* MultimediaManager::GetMPD() {
	return this->mpd;
}
bool    MultimediaManager::Init(const std::string& url) {
	EnterCriticalSection(&this->monitorMutex);

	this->mpd = this->manager->Open((char*)url.c_str());

	if (this->mpd == NULL) {
		LeaveCriticalSection(&this->monitorMutex);
		return false;
	}

	LeaveCriticalSection(&this->monitorMutex);
	return true;
}
void    MultimediaManager::Start() {
	/* Global Start button for start must be added to interface*/
	if (this->isStarted)
		this->Stop();

	EnterCriticalSection(&this->monitorMutex);

	this->NotifyStatusObservers(this->GeneralStatusInformation());

	this->InitVideoRendering(0);
	if (this->videoStream) {
		this->videoStream->Start(this);
		this->StartVideoRenderingThread();

		this->NotifyStatusObservers(this->videoStream->StreamInformation());
	}

	this->InitAudioPlayback(0);
	if (this->audioStream) {
		this->audioElement->StartPlayback();
		this->audioStream->Start();
		this->StartAudioRenderingThread();

		this->NotifyStatusObservers(this->audioStream->StreamInformation());
	}

	this->isStarted = true;

	LeaveCriticalSection(&this->monitorMutex);
}
void    MultimediaManager::Stop() {
	if (!this->isStarted)
		return;

	EnterCriticalSection(&this->monitorMutex);

	this->StopVideo();
	this->StopAudio();

	this->isStarted = false;

	LeaveCriticalSection(&this->monitorMutex);
}
void    MultimediaManager::StopVideo() {
	if (this->isStarted && this->videoStream) {
		this->videoStream->Stop();
		this->StopVideoRenderingThread();

		delete this->videoStream;
		delete this->videoLogic;

		this->videoStream = NULL;
		this->videoLogic = NULL;
	}
}
void    MultimediaManager::StopAudio() {
	if (this->isStarted && this->audioStream) {
		this->audioStream->Stop();
		this->StopAudioRenderingThread();

		this->audioElement->StopPlayback();

		delete this->audioStream;
		delete this->audioLogic;

		this->audioStream = NULL;
		this->audioLogic = NULL;
	}
}
bool    MultimediaManager::SetVideoAdaptationLogic(libdash::framework::adaptation::LogicType type) {
	//Currently unused, always using ManualAdaptation.
	return true;
}
bool    MultimediaManager::SetAudioAdaptationLogic(libdash::framework::adaptation::LogicType type) {
	//Currently unused, always using ManualAdaptation.
	return true;
}
void    MultimediaManager::AttachManagerObserver(IMultimediaManagerObserver* observer) {
	this->managerObservers.push_back(observer);
}
void    MultimediaManager::NotifyVideoBufferObservers(uint32_t fillstateInPercent) {
	for (size_t i = 0; i < this->managerObservers.size(); i++)
		this->managerObservers.at(i)->OnVideoBufferStateChanged(fillstateInPercent);
}
void    MultimediaManager::NotifyVideoSegmentBufferObservers(uint32_t fillstateInPercent) {
	for (size_t i = 0; i < this->managerObservers.size(); i++)
		this->managerObservers.at(i)->OnVideoSegmentBufferStateChanged(fillstateInPercent);
}
void    MultimediaManager::NotifyAudioSegmentBufferObservers(uint32_t fillstateInPercent) {
	for (size_t i = 0; i < this->managerObservers.size(); i++)
		this->managerObservers.at(i)->OnAudioSegmentBufferStateChanged(fillstateInPercent);
}
void    MultimediaManager::NotifyAudioBufferObservers(uint32_t fillstateInPercent) {
	for (size_t i = 0; i < this->managerObservers.size(); i++)
		this->managerObservers.at(i)->OnAudioBufferStateChanged(fillstateInPercent);
}
void    MultimediaManager::NotifyStatusObservers(const std::string& statusInformation) {
	for (size_t i = 0; i < this->managerObservers.size(); i++)
		this->managerObservers.at(i)->OnStatusInformationChanged(statusInformation);
}
void    MultimediaManager::NotifyResolutionChange(int width, int height) {
	std::stringstream text;
	text << "Resolution: " << width << " x " << height;

	for (size_t i = 0; i < this->managerObservers.size(); i++)
		this->managerObservers.at(i)->OnResolutionChanged(text.str());
}
void    MultimediaManager::InitVideoRendering(uint32_t offset) {
	if (this->videoStream || this->mpd == NULL || this->mpd->GetPeriods().empty())
		return;

	IPeriod* period = this->mpd->GetPeriods().at(0);
	std::vector<IAdaptationSet*> videoAdaptationSets = libdash::framework::mpd::AdaptationSetHelper::GetVideoAdaptationSets(period);

	if (videoAdaptationSets.empty())
		return;

	this->videoLogic = AdaptationLogicFactory::Create(libdash::framework::adaptation::Manual, this->mpd, period, videoAdaptationSets.at(0));

	this->videoStream = new MultimediaStream(sampleplayer::managers::VIDEO, this->mpd, SEGMENTBUFFER_SIZE, 2, 0);
	this->videoStream->AttachStreamObserver(this);
	this->videoStream->SetPosition(offset);
}
void    MultimediaManager::InitAudioPlayback(uint32_t offset) {
	if (this->audioStream || this->mpd == NULL || this->mpd->GetPeriods().empty())
		return;

	IPeriod* period = this->mpd->GetPeriods().at(0);
	std::vector<IAdaptationSet*> audioAdaptationSets = libdash::framework::mpd::AdaptationSetHelper::GetAudioAdaptationSets(period);

	if (audioAdaptationSets.empty())
		return;

	this->audioLogic = AdaptationLogicFactory::Create(libdash::framework::adaptation::Manual, this->mpd, period, audioAdaptationSets.at(0));

	this->audioStream = new MultimediaStream(sampleplayer::managers::AUDIO, this->mpd, SEGMENTBUFFER_SIZE, 0, 10);
	this->audioStream->AttachStreamObserver(this);
	this->audioStream->SetPosition(offset);
}
void    MultimediaManager::OnSegmentDownloaded() {
	this->segmentsDownloaded++;
}
void    MultimediaManager::OnSegmentBufferStateChanged(StreamType type, uint32_t fillstateInPercent) {
	switch (type) {
	case AUDIO:
		this->NotifyAudioSegmentBufferObservers(fillstateInPercent);
		break;
	case VIDEO:
		this->NotifyVideoSegmentBufferObservers(fillstateInPercent);
		break;
	default:
		break;
	}
}
void    MultimediaManager::OnVideoBufferStateChanged(uint32_t fillstateInPercent) {
	this->NotifyVideoBufferObservers(fillstateInPercent);
}
void    MultimediaManager::OnAudioBufferStateChanged(uint32_t fillstateInPercent) {
	this->NotifyAudioBufferObservers(fillstateInPercent);
}
void    MultimediaManager::SetFrameRate(double framerate) {
	this->frameRate = framerate;
}
std::string  MultimediaManager::GeneralStatusInformation() {
	std::stringstream text;
	text << "MPD Type = " << this->mpd->GetType() << std::endl;
	text << "SegmentBuffer size = " << SEGMENTBUFFER_SIZE << std::endl;


	//std::cout << text.str();
	return text.str();
}

bool    MultimediaManager::StartVideoRenderingThread() {
	this->isVideoRendering = true;

	this->videoRendererHandle = CreateThreadPortable(RenderVideo, this);

	if (this->videoRendererHandle == NULL)
		return false;

	return true;
}
void    MultimediaManager::StopVideoRenderingThread() {
	this->isVideoRendering = false;

	if (this->videoRendererHandle != NULL) {
		LeaveCriticalSection(&this->monitorMutex);
		JoinThread(this->videoRendererHandle);
		EnterCriticalSection(&this->monitorMutex);

		DestroyThreadPortable(this->videoRendererHandle);
	}
}
bool    MultimediaManager::StartAudioRenderingThread() {
	this->isAudioRendering = true;

	this->audioRendererHandle = CreateThreadPortable(RenderAudio, this);

	if (this->audioRendererHandle == NULL)
		return false;

	return true;
}
void    MultimediaManager::StopAudioRenderingThread() {
	this->isAudioRendering = false;

	if (this->audioRendererHandle != NULL) {
		JoinThread(this->audioRendererHandle);
		DestroyThreadPortable(this->audioRendererHandle);
	}
}
void* MultimediaManager::RenderVideo(void* data) {
	MultimediaManager* manager = (MultimediaManager*)data;
	int width = 0;
	int height = 0;

	QImage* frame = manager->videoStream->GetFrame();

	if (frame) {
		width = frame->width();
		height = frame->height();

		manager->NotifyResolutionChange(width, height);
	}

	while (true) {
		EnterCriticalSection(&manager->monitorMutex);
		if (!manager->isVideoRendering) {
			LeaveCriticalSection(&manager->monitorMutex);
			break;
		}
		double actualFrameRate = manager->frameRate * manager->speed;
		LeaveCriticalSection(&manager->monitorMutex);

		if (frame) {
			manager->videoElement->SetImage(frame);
			manager->videoElement->update();

			manager->framesDisplayed++;

			
			PortableSleep(1.0 / actualFrameRate);

			delete(frame);
		}

		frame = manager->videoStream->GetFrame();

		if (frame) {
			if (width != frame->width() || height != frame->height()) {
				width = frame->width();
				height = frame->height();

				manager->NotifyResolutionChange(width, height);
			}
		}
	}

	return NULL;
}
void* MultimediaManager::RenderAudio(void* data) {
	MultimediaManager* manager = (MultimediaManager*)data;

	AudioChunk* samples = manager->audioStream->GetSamples();

	while (manager->isAudioRendering) {
		if (samples) {
			manager->audioElement->WriteToBuffer(samples->Data(), samples->Length());

			PortableSleep(1 / manager->frameRate);

			delete samples;
		}

		samples = manager->audioStream->GetSamples();
	}

	return NULL;
}
