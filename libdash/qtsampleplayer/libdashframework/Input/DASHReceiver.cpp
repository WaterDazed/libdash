/*
 * DASHReceiver.cpp
 *****************************************************************************
 * Copyright (C) 2012, bitmovin Softwareentwicklung OG, All Rights Reserved
 *
 * Email: libdash-dev@vicky.bitmovin.net
 *
 * This source code and its use and distribution, is subject to the terms
 * and conditions of the applicable license agreement.
 *****************************************************************************/

#include "DASHReceiver.h"
#include "../MPD/AdaptationSetHelper.h"

using namespace libdash::framework::input;
using namespace libdash::framework::buffer;
using namespace libdash::framework::mpd;
using namespace dash::mpd;

DASHReceiver::DASHReceiver(IMPD* mpd, IDASHReceiverObserver* obs, MediaObjectBuffer* buffer, uint32_t bufferSize) :
	mpd(mpd),
	period(NULL),
	adaptationSet(NULL),
	representation(NULL),
	adaptationSetStream(NULL),
	representationStream(NULL),
	segmentNumber(0),
	observer(obs),
	buffer(buffer),
	bufferSize(bufferSize),
	isBuffering(false),
	latestDecodedsegmentNumber(-1),
	shortBandwidth(300),
	longBandwidth(300),
	downloadedBytesStore(0),
	downloadTimeStore(0){
	this->period = this->mpd->GetPeriods().at(0);
	this->adaptationSet = this->period->GetAdaptationSets().at(0);
	this->representation = this->adaptationSet->GetRepresentation().at(0);

	this->adaptationSetStream = new AdaptationSetStream(mpd, period, adaptationSet);
	this->representationStream = adaptationSetStream->GetRepresentationStream(this->representation);
	this->segmentOffset = CalculateSegmentOffset();

	InitializeCriticalSection(&this->monitorMutex);
}
DASHReceiver::~DASHReceiver() {
	delete this->adaptationSetStream;
	DeleteCriticalSection(&this->monitorMutex);
}

bool                        DASHReceiver::Start() {
	if (this->isBuffering)
		return false;

	this->isBuffering = true;
	this->bufferingThread = CreateThreadPortable(DoBuffering, this);

	if (this->bufferingThread == NULL) {
		this->isBuffering = false;
		return false;
	}

	return true;
}
void                        DASHReceiver::Stop() {
	if (!this->isBuffering)
		return;

	this->isBuffering = false;
	this->buffer->SetEOS(true);

	if (this->bufferingThread != NULL) {
		JoinThread(this->bufferingThread);
		DestroyThreadPortable(this->bufferingThread);
	}
}
MediaObject* DASHReceiver::GetNextSegment() {
	ISegment* seg = NULL;

	if (this->segmentNumber >= this->representationStream->GetSize())
		return NULL;

	seg = this->representationStream->GetMediaSegment(this->segmentNumber + this->segmentOffset);

	if (seg != NULL) {
		MediaObject* media = new MediaObject(seg, this->representation, this->segmentNumber);
		this->segmentNumber++;
		return media;
	}

	return NULL;
}
MediaObject* DASHReceiver::GetSegment(uint32_t segNum) {
	ISegment* seg = NULL;

	if (segNum >= this->representationStream->GetSize())
		return NULL;

	seg = this->representationStream->GetMediaSegment(segNum + segmentOffset);

	if (seg != NULL) {
		MediaObject* media = new MediaObject(seg, this->representation);
		return media;
	}

	return NULL;
}
MediaObject* DASHReceiver::GetInitSegment() {
	ISegment* seg = NULL;

	seg = this->representationStream->GetInitializationSegment();

	if (seg != NULL) {
		MediaObject* media = new MediaObject(seg, this->representation);
		return media;
	}

	return NULL;
}
MediaObject* DASHReceiver::FindInitSegment(dash::mpd::IRepresentation* representation) {
	if (!this->InitSegmentExists(representation))
		return NULL;

	return this->initSegments[representation];
}
uint32_t                    DASHReceiver::GetPosition() {
	return this->segmentNumber;
}
void                        DASHReceiver::SetPosition(uint32_t segmentNumber) {
	// some logic here

	this->segmentNumber = segmentNumber;
}
void                        DASHReceiver::SetPositionInMsecs(uint32_t milliSecs) {
	// some logic here

	this->positionInMsecs = milliSecs;
}
void                        DASHReceiver::SetRepresentation(IPeriod* period, IAdaptationSet* adaptationSet, IRepresentation* representation) {
	EnterCriticalSection(&this->monitorMutex);

	bool periodChanged = false;

	if (this->representation == representation) {
		LeaveCriticalSection(&this->monitorMutex);
		return;
	}

	EnterCriticalSection(&this->buffer->monitorMutex);

	while (this->buffer->LengthWithLock())
		this->buffer->PopFrontWithLock();
	this->segmentNumber = (uint32_t)(this->latestDecodedsegmentNumber + 1);

	this->representation = representation;

	if (this->adaptationSet != adaptationSet) {
		this->adaptationSet = adaptationSet;

		if (this->period != period) {
			this->period = period;
			periodChanged = true;
		}

		delete this->adaptationSetStream;
		this->adaptationSetStream = NULL;

		this->adaptationSetStream = new AdaptationSetStream(this->mpd, this->period, this->adaptationSet);
	}

	this->representationStream = this->adaptationSetStream->GetRepresentationStream(this->representation);
	this->DownloadInitSegment(this->representation);

	if (periodChanged) {
		this->segmentNumber = 0;
		this->CalculateSegmentOffset();
	}

	LeaveCriticalSection(&this->buffer->monitorMutex);
	LeaveCriticalSection(&this->monitorMutex);
}
dash::mpd::IRepresentation* DASHReceiver::GetRepresentation() {
	return this->representation;
}
void DASHReceiver::SelectRepresentation() {
	std::vector<IAdaptationSet*>   videoAdaptationSets = AdaptationSetHelper::GetVideoAdaptationSets(period);
	IAdaptationSet* videoAdaptationSet = videoAdaptationSets.at(0);
	const std::vector<IRepresentation*>& videoRepresentations = videoAdaptationSet->GetRepresentation();

	IRepresentation* videoRepresentation = nullptr, * minVideoRepresentation = nullptr;
	double safeBandwidth = 0.9 * min(shortBandwidth, longBandwidth);
	double minBandwidch = 0x3f3f3f3f;
	for (auto it = videoRepresentations.begin(); it != videoRepresentations.end(); it++) {
		IRepresentation* rep = *it;
		double repBandwidth = (double)rep->GetBandwidth() / 8.0 / 1024.0;
		if (repBandwidth <= safeBandwidth) {
			if (!videoRepresentation)
				videoRepresentation = rep;
			else {
				double videoRepresentationBandwidth = (double)videoRepresentation->GetBandwidth() / 8.0 / 1024.0;
				if (repBandwidth > videoRepresentationBandwidth)
					videoRepresentation = rep;
			}
		}
		if (repBandwidth < minBandwidch) {
			minBandwidch = repBandwidth;
			minVideoRepresentation = rep;
		}
	}
	if (!videoRepresentation)
		videoRepresentation = minVideoRepresentation;
	
	EnterCriticalSection(&this->monitorMutex);

	if (this->representation == videoRepresentation) {
		LeaveCriticalSection(&this->monitorMutex);
		return;
	}
		
	EnterCriticalSection(&this->buffer->monitorMutex);

	while (this->buffer->LengthWithLock())
		this->buffer->PopFrontWithLock();
	this->segmentNumber = (uint32_t)(this->latestDecodedsegmentNumber + 1);

	this->representation = videoRepresentation;

	this->representationStream = this->adaptationSetStream->GetRepresentationStream(this->representation);
	this->DownloadInitSegment(this->representation);

	LeaveCriticalSection(&this->buffer->monitorMutex);
	LeaveCriticalSection(&this->monitorMutex);
}
uint32_t                    DASHReceiver::CalculateSegmentOffset() {
	if (mpd->GetType() == "static")
		return 0;

	uint32_t firstSegNum = this->representationStream->GetFirstSegmentNumber();
	uint32_t currSegNum = this->representationStream->GetCurrentSegmentNumber();
	uint32_t startSegNum = currSegNum - 2 * bufferSize;

	return (startSegNum > firstSegNum) ? startSegNum : firstSegNum;
}
void                        DASHReceiver::NotifySegmentDownloaded() {
	this->observer->OnSegmentDownloaded();
}
void                        DASHReceiver::DownloadInitSegment(IRepresentation* rep) {
	if (this->InitSegmentExists(rep))
		return;

	MediaObject* initSeg = NULL;
	initSeg = this->GetInitSegment();

	if (initSeg) {
		initSeg->StartDownload();
		this->initSegments[rep] = initSeg;
	}
}
bool                        DASHReceiver::InitSegmentExists(IRepresentation* rep) {
	if (this->initSegments.find(rep) != this->initSegments.end())
		return true;

	return false;
}
std::string                 DASHReceiver::StatusInformation() {
	std::stringstream text;

	switch (this->representationStream->GetStreamType()) {
	case framework::mpd::SegmentList:
		text << "SegmentList stream" << std::endl;
		break;
	case framework::mpd::SegmentTemplate:
		text << "SegmentTemplate stream" << std::endl;
		break;
	case framework::mpd::SingleMediaSegment:
		text << "BaseUrl stream" << std::endl;
		break;
	}

	text << this->representationStream->StatusInformation();

	return text.str();
}

/* Thread that does the buffering of segments */
void* DASHReceiver::DoBuffering(void* receiver) {
	DASHReceiver* dashReceiver = (DASHReceiver*)receiver;

	dashReceiver->SelectRepresentation();
	dashReceiver->DownloadInitSegment(dashReceiver->GetRepresentation());
	EnterCriticalSection(&dashReceiver->monitorMutex);
	MediaObject* media = dashReceiver->GetNextSegment();
	LeaveCriticalSection(&dashReceiver->monitorMutex);

	while (media != NULL && dashReceiver->isBuffering) {
		media->segment->AttachDownloadObserver(dashReceiver);
		media->StartDownload();
		media->WaitFinished();

		dashReceiver->NotifySegmentDownloaded();

		dashReceiver->SelectRepresentation();
		if (!dashReceiver->buffer->PushBackWithCheck(media, dashReceiver))
			return NULL;
		EnterCriticalSection(&dashReceiver->monitorMutex);
		media = dashReceiver->GetNextSegment();
		LeaveCriticalSection(&dashReceiver->monitorMutex);
	}

	dashReceiver->buffer->SetEOS(true);
	return NULL;
}
void DASHReceiver::OnDownloadRateChanged(uint64_t bytesDownloaded) {

}
void DASHReceiver::OnDownloadStateChanged(dash::network::DownloadState state) {

}
void DASHReceiver::OnDownloadComplete(double downloadedBytes, double downloadTime) {
	//std::cout << '*' << segmentNumber << std::endl;
	double downloadSpeed = downloadedBytes / 1024.0 / downloadTime;
	if (segmentNumber == 1) {
		shortBandwidth = downloadSpeed;
		longBandwidth = downloadSpeed;
		downloadedBytesStore = downloadedBytes;
		downloadTimeStore = downloadTime;
		std::cout << downloadSpeed << " (" << shortBandwidth << ' ' << longBandwidth << ')' << std::endl;
		return;
	}
	downloadedBytesStore += downloadedBytes;
	downloadTimeStore += downloadTime;
	if (!(segmentNumber % 1))
		shortBandwidth = downloadSpeed * 0.3 + shortBandwidth * 0.7;
	if (!(segmentNumber % 3)) {
		longBandwidth = downloadSpeed * 0.3 + longBandwidth * 0.7;
		downloadedBytesStore = 0;
		downloadTimeStore = 0;
	}
	std::cout << downloadSpeed << " (" << shortBandwidth << ' ' << longBandwidth << ')' << std::endl;
}