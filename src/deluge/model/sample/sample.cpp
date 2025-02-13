/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "model/sample/sample.h"
#include "NE10.h"
#include "definitions_cxx.hpp"
#include "dsp/fft/fft_config_manager.h"
#include "dsp/timestretch/time_stretcher.h"
#include "hid/display/display.h"
#include "io/debug/print.h"
#include "memory/general_memory_allocator.h"
#include "model/sample/sample_cache.h"
#include "model/sample/sample_perc_cache_zone.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/multi_range/multisample_range.h"
#include "storage/storage_manager.h"
#include "util/functions.h"
#include "util/lookuptables/lookuptables.h"
#include <cstdint>
#include <math.h>
#include <new>
#include <string.h>

extern "C" {
#include "RZA1/uart/sio_char.h"
}

#if SAMPLE_DO_LOCKS
#define LOCK_ENTRY                                                                                                     \
	if (lock) {                                                                                                        \
		display->freezeWithError("i024");                                                                              \
	}                                                                                                                  \
	lock = true;
#define LOCK_EXIT lock = false;
#else
#define LOCK_ENTRY                                                                                                     \
	{}
#define LOCK_EXIT                                                                                                      \
	{}
#endif

struct SampleCacheElement {
	int32_t phaseIncrement;
	int32_t timeStretchRatio;
	int32_t skipSamplesAtStart;
	uint32_t reversed; // Bool would be fine, but got to make it 32-bit for OrderedResizeableArrayWithMultiWordKey
	SampleCache* cache;
};

Sample::Sample()
    : percCacheZones{sizeof(SamplePercCacheZone), sizeof(SamplePercCacheZone)}, caches(sizeof(SampleCacheElement), 4),
      AudioFile(AudioFileType::SAMPLE) {
	audioDataLengthBytes = 0;
	audioDataStartPosBytes = 0;
	lengthInSamples = 0;
	rawDataFormat = RAW_DATA_FINE;
	midiNote = MIDI_NOTE_UNSET;
	partOfFolderBeingLoaded = false;

	minValueFound = 2147483647;
	maxValueFound = -2147483648;

	percCacheMemory[0] = NULL;
	percCacheMemory[1] = NULL;

	percCacheClusters[0] = NULL;
	percCacheClusters[1] = NULL;

	fileLoopStartSamples = 0;
	fileLoopEndSamples = 0;
	midiNoteFromFile = -1;

	beginningOffsetForPitchDetection = 0;
	beginningOffsetForPitchDetectionFound = false;

#if SAMPLE_DO_LOCKS
	lock = false;
#endif
}

int32_t Sample::initialize(int32_t newNumClusters) {
	unloadable = false;
	unplayable = false;
	waveTableCycleSize = 2048; // Default
	fileExplicitlySpecifiesSelfAsWaveTable = false;

	return clusters.insertSampleClustersAtEnd(newNumClusters);
}

Sample::~Sample() {
	for (int32_t c = 0; c < clusters.getNumElements(); c++) {
		clusters.getElement(c)->~SampleCluster();
	}

	deletePercCache(true);

	for (int32_t i = 0; i < caches.getNumElements(); i++) {
		SampleCacheElement* element = (SampleCacheElement*)caches.getElementAddress(i);
		element->cache->~SampleCache();
		delugeDealloc(element->cache);
	}
}

void Sample::deletePercCache(bool beingDestructed) {

	for (int32_t reversed = 0; reversed < 2; reversed++) {
		if (percCacheMemory[reversed]) {
			delugeDealloc(percCacheMemory[reversed]);
			if (!beingDestructed) {
				percCacheMemory[reversed] = NULL;
			}
		}

		if (percCacheClusters[reversed]) {

			for (int32_t c = 0; c < numPercCacheClusters; c++) {
				if (percCacheClusters[reversed][c]) {

					// If any of them still has a "reason", well, it shouldn't
					if (ALPHA_OR_BETA_VERSION && percCacheClusters[reversed][c]->numReasonsToBeLoaded) {
						display->freezeWithError("E137");
					}

					audioFileManager.deallocateCluster(percCacheClusters[reversed][c]);
					// Don't bother actually setting our pointer to NULL, cos we're about to deallocate that memory anyway
				}
			}

			delugeDealloc(percCacheClusters[reversed]);
			if (!beingDestructed) {
				percCacheClusters[reversed] = NULL;
			}
		}

		if (!beingDestructed) {
			percCacheZones[reversed].empty();
		}
	}
}

void Sample::workOutBitMask() {
	bitMask = 0xFFFFFFFF << ((4 - byteDepth) * 8);
}

void Sample::markAsUnloadable() {
	unloadable = true;

	// If any Clusters in the load-queue, remove them from there
	for (int32_t c = 0; c < clusters.getNumElements(); c++) {
		Cluster* cluster = clusters.getElement(c)->cluster;
		if (cluster) {
			if (audioFileManager.loadingQueue.removeIfPresent(cluster)) {
				// TODO: what's going to happen to this cluster now?
			}
		}
	}
}

SampleCache* Sample::getOrCreateCache(SampleHolder* sampleHolder, int32_t phaseIncrement, int32_t timeStretchRatio,
                                      bool reversed, bool mayCreate, bool* created) {

	int32_t skipSamplesAtStart;
	if (!reversed) {
		skipSamplesAtStart = sampleHolder->startPos;
	}
	else {
		skipSamplesAtStart = lengthInSamples - sampleHolder->getEndPos(false);
	}

	uint32_t keyWords[4];
	keyWords[0] = phaseIncrement;
	keyWords[1] = timeStretchRatio;
	keyWords[2] = skipSamplesAtStart;
	keyWords[3] = reversed;
	int32_t i = caches.searchMultiWordExact(keyWords);

	// If it already existed...
	if (i != -1) {
		*created = false;
		SampleCacheElement* element = (SampleCacheElement*)caches.getElementAddress(i);
		return element->cache;
	}

	// Or if still here, it didn't already exist.
	if (!mayCreate) {
		return NULL;
	}

	uint64_t combinedIncrement = ((uint64_t)(uint32_t)phaseIncrement * (uint32_t)timeStretchRatio) >> 24;

	uint64_t lengthInSamplesCached = ((uint64_t)(lengthInSamples - skipSamplesAtStart) << 24) / combinedIncrement
	                                 + 1; // Not 100% sure on the +1, but better safe than sorry

	// Make it a bit longer, to capture the ring-out of the interpolation / time-stretching
	if (phaseIncrement != 16777216) {
		lengthInSamplesCached += (kInterpolationMaxNumSamples >> 1);
	}
	if (timeStretchRatio != 16777216) {
		lengthInSamplesCached += 16384; // This one is quite an inexact science
	}

	uint64_t lengthInBytesCached = lengthInSamplesCached * kCacheByteDepth * numChannels;

	if (lengthInBytesCached >= (32 << 20)) {
		return NULL; // If cache would be more than 32MB, assume that it wouldn't be very useful to cache it
	}

	int32_t numClusters = ((lengthInBytesCached - 1) >> audioFileManager.clusterSizeMagnitude) + 1;
	void* memory = GeneralMemoryAllocator::get().alloc(sizeof(SampleCache) + (numClusters - 1) * sizeof(Cluster*), NULL,
	                                                   false, false);
	if (!memory) {
		return NULL;
	}

	i = caches.insertAtKeyMultiWord(keyWords);
	if (i == -1) { // If error
		delugeDealloc(memory);
		return NULL;
	}

	SampleCache* samplePitchAdjustment = new (memory)
	    SampleCache(this, numClusters, lengthInBytesCached, phaseIncrement, timeStretchRatio, skipSamplesAtStart);

	SampleCacheElement* element = (SampleCacheElement*)caches.getElementAddress(i);
	element->phaseIncrement = phaseIncrement;
	element->timeStretchRatio = timeStretchRatio;
	element->cache = samplePitchAdjustment;
	element->skipSamplesAtStart = skipSamplesAtStart;
	element->reversed = reversed;

	*created = true;
	return samplePitchAdjustment;
}

void Sample::deleteCache(SampleCache* cache) {
	// Not currently used anymore
	/*
	int32_t i = pitchAdjustmentCaches.search(cache->phaseIncrement, cache->skipSamplesAtStart, cache->reversed, GREATER_OR_EQUAL);
	if (i < pitchAdjustmentCaches.getNumElements()) {
		SampleCacheElement* element = pitchAdjustmentCaches.getElement(i);

		if (element->phaseIncrement != cache->phaseIncrement
				|| element->skipSamplesAtStart != cache->skipSamplesAtStart
				|| element->reversed != cache->reversed) return;

		cache->~SampleCache();
		pitchAdjustmentCaches.deleteElement(i);
		Debug::println("cache deleted");
	}
	*/
}

#define MEASURE_PERC_CACHE_PERFORMANCE 0

// Returns error
int32_t Sample::fillPercCache(TimeStretcher* timeStretcher, int32_t startPosSamples, int32_t endPosSamples,
                              int32_t playDirection, int32_t maxNumSamplesToProcess) {

#if MEASURE_PERC_CACHE_PERFORMANCE
	uint16_t startTime = MTU2.TCNT_0;
#endif

	int32_t reversed = (playDirection == 1) ? 0 : 1;

	// If the start pos is already beyond the waveform, we can get out right now!
	if (!reversed) {
		if (startPosSamples >= lengthInSamples) {
			return NO_ERROR;
		}
	}
	else {
		if (startPosSamples < 0) {
			return NO_ERROR;
		}
	}

	LOCK_ENTRY

	AudioEngine::logAction("fillPercCache");

	//int32_t lengthInSamplesAfterReduction = ((lengthInSamples + (kPercBufferReductionSize >> 1)) >> PERC_BUFFER_REDUCTION_MAGNITUDE);
	int32_t lengthInSamplesAfterReduction = ((lengthInSamples - 1) >> kPercBufferReductionMagnitude) + 1;
	lengthInSamplesAfterReduction = std::max(lengthInSamplesAfterReduction, 1_i32); // Can't allocate less than 1 byte

	bool percCacheDoneWithClusters = (lengthInSamplesAfterReduction >= (audioFileManager.clusterSize >> 1));

	if (percCacheDoneWithClusters) {
		if (!percCacheClusters[reversed]) {
			numPercCacheClusters = ((lengthInSamplesAfterReduction - 1) >> audioFileManager.clusterSizeMagnitude)
			                       + 1; // Stores this number for the future too
			int32_t memorySize = numPercCacheClusters * sizeof(Cluster*);
			percCacheClusters[reversed] = (Cluster**)GeneralMemoryAllocator::get().alloc(memorySize, NULL, false, true);
			if (!percCacheClusters[reversed]) {
				LOCK_EXIT
				return ERROR_INSUFFICIENT_RAM;
			}

			memset(percCacheClusters[reversed], 0, memorySize);
		}
	}

	else {

		if (!percCacheMemory[reversed]) {
			int32_t percCacheSize = lengthInSamplesAfterReduction;

			percCacheMemory[reversed] = (uint8_t*)GeneralMemoryAllocator::get().alloc(percCacheSize);
			if (!percCacheMemory[reversed]) {
				LOCK_EXIT
				return ERROR_INSUFFICIENT_RAM;
			}

			//Debug::println("allocated percCacheMemory");
		}
	}

	int32_t bytesPerSample = numChannels * byteDepth;
	int32_t posIncrement = bytesPerSample * playDirection;

	int32_t i;
	if (!reversed) {
		i = percCacheZones[reversed].search(startPosSamples + 1, LESS);
	}
	else {
		i = percCacheZones[reversed].search(startPosSamples, GREATER_OR_EQUAL);
	}

	int32_t error = NO_ERROR;
	SamplePercCacheZone* percCacheZone;
	if (i >= 0 && i < percCacheZones[reversed].getNumElements()) {
		percCacheZone = (SamplePercCacheZone*)percCacheZones[reversed].getElementAddress(i);

		// Primarily, we check here whether this zone ends after our start-pos. However, we also test positive if the zone's end is *almost* as far along
		// as our start-pos but not quite. In such a case, it still makes sense to continue adding to that zone, starting a little further back than
		// we had planned to. This prevents the situation where time-stretching is on extremely fast and each call to this function is so much further
		// along that a new zone is created every time, leading to thousands of zones, so huge overhead each time we want to insert or delete. Instead, this
		// new method will cause the zones to clump together, or better yet just manage to cover the whole area in one zone. This is far more efficient in every
		// way - remember that each zone will have a number of samplesAtStartWhichShouldBeReplaced, so ending up with thousands of zones is just a terrible
		// idea.
		if ((percCacheZone->endPos - startPosSamples) * playDirection
		    >= -2048) { // -2048 helps massively. Not sure if we can go lower. Also tested -4096 - same result. Not fine-tuned beyond that

			// Reset startPosSamples back to the zone endPos, which may have been a bit further back. That's the place where we're
			// guaranteed that there's still a perc cache Cluster (I think? Or unless it's the first sample of a new one?)
			startPosSamples = percCacheZone->endPos; // This can end up as -1! Because endPos can - see its comment.

			// If the (potentially made-later) start pos is already beyond the waveform, get out (otherwise we'd be prone to an error getting the perc Cluster below. Fixed Aug 2021
			if (!reversed) {
				if (startPosSamples >= lengthInSamples) {
doReturnNoError:
					LOCK_EXIT
					return NO_ERROR;
				}
			}
			else {
				if (startPosSamples < 0) {
					goto doReturnNoError;
				}
			}

			// First, update our "current pos for perc cache filling and reading" sorta thing so
			// no one steals the first Cluster we're gonna need. This is especially important for just now while we're gonna be reading
			// some of this Cluster, but also we want to keep it in memory for next time we come back here.
			int32_t percClusterIndexStart;
			if (percCacheDoneWithClusters) {
				percClusterIndexStart = (uint32_t)startPosSamples
				                        >> (audioFileManager.clusterSizeMagnitude + kPercBufferReductionMagnitude);
				if (ALPHA_OR_BETA_VERSION && percClusterIndexStart >= numPercCacheClusters) {
					display->freezeWithError("E138");
				}
				Cluster* clusterHere = percCacheClusters[reversed][percClusterIndexStart];
#if ALPHA_OR_BETA_VERSION
				if (!clusterHere) {

					// That's actually allowed if we're right at the start of that cluster. But otherwise...
					if (startPosSamples
					    & ((1 << audioFileManager.clusterSizeMagnitude + kPercBufferReductionMagnitude) - 1)) {
						// If Cluster has been stolen, the zones should have been updated, so we shouldn't be here
						Debug::println(startPosSamples);
						display->freezeWithError("E139");
					}
				}
#endif
				if (clusterHere) {
					timeStretcher->rememberPercCacheCluster(
					    clusterHere); // If at start of new cluster, there might not be one allocated here yet
				}
			}

			// If it ends after our end-pos too, we're done
			if ((percCacheZone->endPos - endPosSamples) * playDirection >= 0) {

				// But first, if our perc cache is done with Clusters, see if our endPos has a different perc cache Cluster than our startPos, and if so, store it.
				// (It won't be more than 1 Cluster ahead, cos remember the data is so compacted that each perc cache Cluster stores like 90 seconds.)
				if (percCacheDoneWithClusters) {

					// I think the fact that we subtract playDirection here means that we look at the cluster for the very last existing sample, so even
					// if we've actually filled up right up to the cluster boundary but not allocated a next one, it should be fine, ya know?
					int32_t percClusterIndexEnd =
					    (uint32_t)(endPosSamples - playDirection)
					    >> (audioFileManager.clusterSizeMagnitude + kPercBufferReductionMagnitude);
					if (percClusterIndexEnd != percClusterIndexStart) {
#if ALPHA_OR_BETA_VERSION
						if (percClusterIndexEnd >= numPercCacheClusters) {
							display->freezeWithError("E140");
						}
						if (!percCacheClusters[reversed][percClusterIndexEnd]) {
							display->freezeWithError(
							    "E141"); // If Cluster has been stolen, the zones should have been updated, so we shouldn't be here
						}
#endif
						timeStretcher->rememberPercCacheCluster(percCacheClusters[reversed][percClusterIndexEnd]);
					}
				}

				// We're now guaranteed to have a bunch of perc cache secured in RAM, un-stealable. So we can take a breather and know we won't need access to the source Clusters for it anytime very soon
				timeStretcher->unassignAllReasonsForPercLookahead();

				goto doReturnNoError;
			}

			// Or if it ends before our end-pos, we need to add to it
			else {
				goto doLoading;
			}
		}
	}

	// If still here, need to create element. And we know that perc cache Clusters will be allocated and remembered if necessary
	if (!reversed) {
		i++;
	}

	error = percCacheZones[reversed].insertAtIndex(
	    i, 1,
	    this); // Tell it not to steal other perc cache zones from this Sample, which would result in modification of the same array during operation.
	// Fortunately it also has a lock to alert if that actually somehow happened, too.
	if (error) {
		LOCK_EXIT
		return error;
	}

	percCacheZone = new (percCacheZones[reversed].getElementAddress(i)) SamplePercCacheZone(startPosSamples);

doLoading:

#if MEASURE_PERC_CACHE_PERFORMANCE
	int32_t length = (endPosSamples - startPosSamples) * playDirection;
	if (length < kPercBufferReductionSize * 16)
		goto doReturnResult;
	else
		endPosSamples = startPosSamples + kPercBufferReductionSize * 16 * playDirection;
#endif

	// Make sure we don't shoot past end of waveform
	if (!reversed) {
		endPosSamples = std::min(endPosSamples, (int32_t)lengthInSamples);
	}
	else {
		endPosSamples = std::max(endPosSamples, (int32_t)-1);
	}

#if !MEASURE_PERC_CACHE_PERFORMANCE
	int32_t endPosSamplesLimit0 = startPosSamples + maxNumSamplesToProcess * playDirection;
	if ((endPosSamples - endPosSamplesLimit0) * playDirection >= 0) {
		endPosSamples = endPosSamplesLimit0;
	}
#endif

	// See if there's a next element which we should stop before
	int32_t iNext = i + playDirection;
	bool willHitNextElement = false;
	int32_t endPosSamplesLimit;
	SamplePercCacheZone* nextPercCacheZone;
	if (iNext >= 0 && iNext < percCacheZones[reversed].getNumElements()) {
		nextPercCacheZone = (SamplePercCacheZone*)percCacheZones[reversed].getElementAddress(iNext);
		if ((endPosSamples - nextPercCacheZone->startPos) * playDirection >= 0) {
			willHitNextElement = true;

			endPosSamplesLimit =
			    nextPercCacheZone->startPos + nextPercCacheZone->samplesAtStartWhichShouldBeReplaced * playDirection;

			if ((endPosSamples - endPosSamplesLimit) * playDirection >= 0) {
				// TODO: what if that next zone doesn't extend all the way to the end we want?
				endPosSamples = endPosSamplesLimit;
			}
		}
	}

	int32_t sourceBytePos;
	int32_t numSamples = (endPosSamples - startPosSamples) * playDirection;
	if (numSamples <= 0) {
		goto getOut; // This probably would have already been dealt with above - not quite sure
	}
	sourceBytePos = audioDataStartPosBytes + startPosSamples * bytesPerSample;

	do {

		int32_t numSamplesThisClusterReadWrite = numSamples;

		int32_t sourceClusterIndex = sourceBytePos >> audioFileManager.clusterSizeMagnitude;

		if (sourceClusterIndex >= getFirstClusterIndexWithNoAudioData()
		    || sourceClusterIndex
		           < getFirstClusterIndexWithAudioData()) { // Wait, this shouldn't actually happen right?
			goto getOut;
		}

		uint8_t* percCacheNow;
		if (percCacheDoneWithClusters) {
			int32_t percClusterIndex =
			    startPosSamples >> (audioFileManager.clusterSizeMagnitude + kPercBufferReductionMagnitude);
			if (ALPHA_OR_BETA_VERSION && percClusterIndex >= numPercCacheClusters) {
				display->freezeWithError("E136");
			}
			if (!percCacheClusters[reversed][percClusterIndex]) {
				//Debug::println("allocating perc cache Cluster!!!!!!!!!!!!!!!!!!!!!!!!!!!!");
				// We tell it not to steal any other per cache Cluster from this Sample - not because those Clusters are definitely a high priority to keep, but
				// because doing so would probably alter our percCacheZones, which we're currently working with, which could really muck things up. Scenario only discovered Jan 2021.
				percCacheClusters[reversed][percClusterIndex] = audioFileManager.allocateCluster(
				    reversed ? ClusterType::PERC_CACHE_REVERSED : ClusterType::PERC_CACHE_FORWARDS, false,
				    this); // Doesn't add reason. Call to rememberPercCacheCluster() below will
				if (!percCacheClusters[reversed][percClusterIndex]) {
					error = ERROR_INSUFFICIENT_RAM;
					goto getOut;
				}

				percCacheClusters[reversed][percClusterIndex]->sample = this;
				percCacheClusters[reversed][percClusterIndex]->clusterIndex = percClusterIndex;
			}

			timeStretcher->rememberPercCacheCluster(percCacheClusters[reversed][percClusterIndex]);

			percCacheNow = (uint8_t*)percCacheClusters[reversed][percClusterIndex]->data
			               - (percClusterIndex * audioFileManager.clusterSize);

			int32_t posWithinPercClusterBig =
			    startPosSamples & ((audioFileManager.clusterSize << kPercBufferReductionMagnitude) - 1);

			// Bytes and samples are the same for the dest Cluster
			int32_t samplesLeftThisDestCluster =
			    reversed ? (posWithinPercClusterBig + 1)
			             : ((audioFileManager.clusterSize << kPercBufferReductionMagnitude) - posWithinPercClusterBig);
			numSamplesThisClusterReadWrite = std::min(numSamplesThisClusterReadWrite, samplesLeftThisDestCluster);
		}

		else {
			percCacheNow = percCacheMemory[reversed];
		}

		Cluster* cluster =
		    clusters.getElement(sourceClusterIndex)
		        ->cluster; // Don't call getcluster() - that would add a reason, and potentially do loading and stuff.
		if (!cluster || !cluster->loaded) {
			goto getOut;
		}

		int32_t bytePosWithinCluster = sourceBytePos & (audioFileManager.clusterSize - 1);

		// Ok, how many samples can we load right now?
		int32_t bytesLeftThisSourceCluster =
		    reversed ? (bytePosWithinCluster + bytesPerSample)
		             : (audioFileManager.clusterSize - bytePosWithinCluster + bytesPerSample - 1);
		int32_t bytesWeWantToRead = numSamplesThisClusterReadWrite * bytesPerSample;
		if (bytesWeWantToRead > bytesLeftThisSourceCluster + bytesPerSample) {
			numSamplesThisClusterReadWrite = bytesLeftThisSourceCluster / bytesPerSample;
		}

		// Do some stuff here kind of ahead of time, before we go and decrement numSamplesThisClusterReadWrite
		numSamples -= numSamplesThisClusterReadWrite;
		percCacheZone->endPos +=
		    numSamplesThisClusterReadWrite * playDirection; // Do this now, in case the next Cluster fails
		sourceBytePos += numSamplesThisClusterReadWrite * posIncrement;

		// Alright, load those samples
		char* currentPos = (char*)&cluster->data[bytePosWithinCluster] - 4 + byteDepth;

		do {
			int32_t numSamplesThisPercPixelSegment = numSamplesThisClusterReadWrite;

			int32_t numSamplesLeftThisPercPixelSegment =
			    reversed ? (startPosSamples + 1 + (kPercBufferReductionSize >> 1)) & (kPercBufferReductionSize - 1)
			             : kPercBufferReductionSize
			                   - ((startPosSamples + (kPercBufferReductionSize >> 1)) & (kPercBufferReductionSize - 1));

			if (!numSamplesLeftThisPercPixelSegment) {
				numSamplesLeftThisPercPixelSegment = kPercBufferReductionSize;
			}

			numSamplesThisPercPixelSegment =
			    std::min(numSamplesThisPercPixelSegment, numSamplesLeftThisPercPixelSegment);

			char* endPos = currentPos + numSamplesThisPercPixelSegment * posIncrement;

			int32_t angle;

			while (
			    true) { // I've put reasonable effort into benchmarking / optimizing this loop - I don't think it can be improved much more
				int32_t thisSampleRead =
				    *(int32_t*)currentPos
				    >> 2; // Have to make it smaller even if only one, so the "angle" doesn't overflow
				if (numChannels == 2) {
					thisSampleRead += *(int32_t*)(currentPos + byteDepth) >> 2;
				}

				angle = thisSampleRead - percCacheZone->lastSampleRead;
				percCacheZone->lastSampleRead = thisSampleRead;
				if (angle < 0) {
					angle = -angle;
				}

				for (auto& pole : percCacheZone->angleLPFMem) {
					int32_t distanceToGo = angle - pole;
					pole += distanceToGo
					        >> 9; //multiply_32x32_rshift32_rounded(distanceToGo, 1 << 23); //distanceToGo >> 9;
					angle = pole;
				}

				currentPos += posIncrement;
				if (currentPos == endPos) {
					break;
				}

				percCacheZone->lastAngle = angle; // This gets skipped for the last one - and done below
			}

			startPosSamples += numSamplesThisPercPixelSegment * playDirection;

			int32_t posWithinPercPixel = startPosSamples & (kPercBufferReductionSize - 1);

			if (posWithinPercPixel == (kPercBufferReductionSize >> 1) - reversed) {

				int32_t difference = angle - percCacheZone->lastAngle;
				if (difference < 0) {
					difference = -difference;
				}

				int32_t percussiveness = ((uint64_t)difference * 262144 / angle) >> 1;

				percussiveness = getTanH<23>(percussiveness);

				percCacheNow[startPosSamples >> kPercBufferReductionMagnitude] = percussiveness;
			}

			percCacheZone->lastAngle = angle;

			numSamplesThisClusterReadWrite -= numSamplesThisPercPixelSegment;
		} while (numSamplesThisClusterReadWrite);

	} while (numSamples);

	percCacheZone->samplesAtStartWhichShouldBeReplaced =
	    std::max<int32_t>(2048, // 2048 is fairly arbitrary
	                      (percCacheZone->endPos - percCacheZone->startPos) * playDirection);

	// If we connected up to another, later zone...
	if (willHitNextElement) {

		// If we've extended past the samples at start which should be replaced...
		if ((endPosSamples - endPosSamplesLimit) * playDirection >= 0) {
			nextPercCacheZone->startPos = percCacheZone->startPos;
			nextPercCacheZone->samplesAtStartWhichShouldBeReplaced = percCacheZone->samplesAtStartWhichShouldBeReplaced;
			percCacheZones[reversed].deleteAtIndex(i);
		}

		// Or if not...
		else {
			nextPercCacheZone->samplesAtStartWhichShouldBeReplaced -=
			    (endPosSamples - nextPercCacheZone->startPos) * playDirection;
			nextPercCacheZone->startPos = endPosSamples;
		}
	}

	// TODO: what if that next zone doesn't extend all the way to the end we want? Though that'd only very rarely happen, and only hold us up very briefly

#if MEASURE_PERC_CACHE_PERFORMANCE
	{
		uint16_t endTime = MTU2.TCNT_0;
		uint16_t timeTaken = endTime - startTime;
		Debug::print("perc cache fill time: ");
		Debug::println(timeTaken);
	}
#endif

getOut:
	// If we failed to do the loading we wanted to, e.g. because of insufficient RAM, we need to make sure we didn't leave a 0-length zone, cos that's invalid.
	if (percCacheZone->endPos == percCacheZone->startPos) {
		percCacheZones[reversed].deleteAtIndex(i);
	}

	// Unlock now that we've finished dealing with the percCacheZones array. If the below call to updateClustersForPercLookahead() wants to steal any perc cache Clusters
	// and consequently modify that array, it's allowed to.
	LOCK_EXIT

	// If current source Cluster has changed, update TimeStretcher's queue
	timeStretcher->updateClustersForPercLookahead(this, sourceBytePos, playDirection);

	AudioEngine::logAction("/fillPercCache");
	return error; // Usually it'll be NO_ERROR.
}

bool Sample::getAveragesForCrossfade(int32_t* totals, int32_t startBytePos, int32_t crossfadeLengthSamples,
                                     int32_t playDirection, int32_t lengthToAverageEach) {

	int32_t byteDepthNow = byteDepth;
	int32_t numChannelsNow = numChannels;
	int32_t bytesPerSample = byteDepthNow * numChannelsNow;

	// This can happen. Not 100% sure if it should, but we'll return false just below in this case anyway, so I think it's ok
	if (ALPHA_OR_BETA_VERSION && startBytePos < (int32_t)audioDataStartPosBytes) {
		display->freezeWithError("E283");
	}

	int32_t startSamplePos = (uint32_t)(startBytePos - audioDataStartPosBytes) / (uint8_t)bytesPerSample;

	int32_t halfCrossfadeLengthSamples = crossfadeLengthSamples >> 1;

	int32_t samplePosMidCrossfade = startSamplePos + halfCrossfadeLengthSamples * playDirection;

	int32_t readSample = samplePosMidCrossfade
	                     - ((lengthToAverageEach * TimeStretch::Crossfade::kNumMovingAverages) >> 1) * playDirection;

	int32_t halfCrossfadeLengthBytes = halfCrossfadeLengthSamples * bytesPerSample;

	int32_t readByte = readSample * bytesPerSample + audioDataStartPosBytes;

	if (playDirection == 1) {
		if (readByte < (int32_t)audioDataStartPosBytes + halfCrossfadeLengthBytes) {
			return false;
		}
		else if (readByte >= (int32_t)(audioDataStartPosBytes + audioDataLengthBytes - halfCrossfadeLengthBytes)) {
			return false;
		}
	}

	int32_t endReadByte =
	    readByte + lengthToAverageEach * TimeStretch::Crossfade::kNumMovingAverages * bytesPerSample * playDirection;

	if (endReadByte < (int32_t)(audioDataStartPosBytes - 1)
	    || endReadByte > (int32_t)(audioDataStartPosBytes + audioDataLengthBytes)) {
		return false;
	}

	for (int32_t i = 0; i < TimeStretch::Crossfade::kNumMovingAverages; i++) {

		int32_t numSamplesLeftThisAverage = lengthToAverageEach;
		totals[i] = 0;

		if (ALPHA_OR_BETA_VERSION
		    && (readByte < audioDataStartPosBytes - 1 || readByte >= audioDataStartPosBytes + audioDataLengthBytes)) {
			display->freezeWithError("FFFF");
		}

		do {

			if (ALPHA_OR_BETA_VERSION
			    && (readByte < audioDataStartPosBytes - 1
			        || readByte >= audioDataStartPosBytes + audioDataLengthBytes)) {
				display->freezeWithError("E432"); // Was "GGGG". Sven may have gotten.
			}

			int32_t whichCluster = readByte >> audioFileManager.clusterSizeMagnitude;
			if (ALPHA_OR_BETA_VERSION
			    && (whichCluster < getFirstClusterIndexWithAudioData()
			        || whichCluster >= getFirstClusterIndexWithNoAudioData())) {
				display->freezeWithError("EEEE");
			}

			Cluster* cluster = clusters.getElement(whichCluster)->cluster;
			if (!cluster || !cluster->loaded) {
				return false;
			}

			int32_t bytePosWithinCluster = readByte & (audioFileManager.clusterSize - 1);
			int32_t numSamplesThisRead = numSamplesLeftThisAverage;

			int32_t bytesLeftThisCluster =
			    (playDirection == -1) ? (bytePosWithinCluster + bytesPerSample)
			                          : (audioFileManager.clusterSize - bytePosWithinCluster + bytesPerSample - 1);
			int32_t bytesWeWantToRead = numSamplesThisRead * bytesPerSample;
			if (bytesWeWantToRead > bytesLeftThisCluster) {
				numSamplesThisRead = (uint32_t)bytesLeftThisCluster / (uint8_t)bytesPerSample;
			}

			// Alright, read those samples
			char* currentPos = (char*)&cluster->data[bytePosWithinCluster] - 4 + byteDepthNow;
			char* endPos = currentPos + numSamplesThisRead * bytesPerSample * playDirection;

			do {
				totals[i] += *(int32_t*)currentPos >> 16;
				if (numChannelsNow == 2) {
					totals[i] += *(int32_t*)(currentPos + byteDepthNow) >> 16;
				}

				currentPos += bytesPerSample * playDirection;
			} while (currentPos != endPos);

			readByte += numSamplesThisRead * bytesPerSample * playDirection;
			numSamplesLeftThisAverage -= numSamplesThisRead;
			if (ALPHA_OR_BETA_VERSION && numSamplesLeftThisAverage < 0) {
				display->freezeWithError("DDDD");
			}
		} while (numSamplesLeftThisAverage);
	}

	return true;
}

uint8_t* Sample::prepareToReadPercCache(int32_t pixellatedPos, int32_t playDirection, int32_t* earliestPixellatedPos,
                                        int32_t* latestPixellatedPos) {

	int32_t reversed = (playDirection == 1) ? 0 : 1;

	int32_t realPos = (pixellatedPos << kPercBufferReductionMagnitude) + (kPercBufferReductionSize >> 1);
	int32_t i = percCacheZones[reversed].search(realPos + 1 - reversed, reversed ? GREATER_OR_EQUAL : LESS);
	if (i < 0 || i >= percCacheZones[reversed].getNumElements()) {
		return NULL;
	}

	SamplePercCacheZone* zone = (SamplePercCacheZone*)percCacheZones[reversed].getElementAddress(i);
	if ((zone->endPos - realPos) * playDirection <= 0) {
		return NULL;
	}

	*earliestPixellatedPos =
	    (zone->startPos + (kPercBufferReductionSize >> 1) * playDirection) >> kPercBufferReductionMagnitude;
	*latestPixellatedPos =
	    (zone->endPos - (kPercBufferReductionSize >> 1) * playDirection) >> kPercBufferReductionMagnitude;

	// If permanently allocated perc cache...
	if (percCacheMemory[reversed]) {
		return percCacheMemory[reversed];
	}

	// Or if Cluster-based perc cache...
	else {
		int32_t ourCluster = pixellatedPos >> audioFileManager.clusterSizeMagnitude;
		if (ALPHA_OR_BETA_VERSION && !percCacheClusters[reversed][ourCluster]) {
			display->freezeWithError("E142");
		}

		int32_t earliestCluster = *earliestPixellatedPos >> audioFileManager.clusterSizeMagnitude;
		;
		int32_t latestCluster = *latestPixellatedPos >> audioFileManager.clusterSizeMagnitude;

		// Constrain to Cluster boundaries. This will theoretically hurt the sound a tiny bit... once every 90 seconds. No one will ever know
		if (earliestCluster < ourCluster) {
			*earliestPixellatedPos = ourCluster << audioFileManager.clusterSizeMagnitude;
		}
		else if (earliestCluster > ourCluster) {
			*earliestPixellatedPos = ((ourCluster + 1) << audioFileManager.clusterSizeMagnitude) - 1;
		}

		if (latestCluster < ourCluster) {
			*latestPixellatedPos = ourCluster << audioFileManager.clusterSizeMagnitude;
		}
		else if (latestCluster > ourCluster) {
			*latestPixellatedPos = ((ourCluster + 1) << audioFileManager.clusterSizeMagnitude) - 1;
		}

		// Fudge an address to send back
		return (uint8_t*)percCacheClusters[reversed][ourCluster]->data - (ourCluster * audioFileManager.clusterSize);
	}
}

void Sample::percCacheClusterStolen(Cluster* cluster) {
	LOCK_ENTRY

	Debug::println("percCacheClusterStolen -----------------------------------------------------------!!");
	int32_t reversed = (cluster->type == ClusterType::PERC_CACHE_REVERSED);
	int32_t playDirection = reversed ? -1 : 1;
	int32_t comparison = reversed ? GREATER_OR_EQUAL : LESS;

#if ALPHA_OR_BETA_VERSION
	if (cluster->type != ClusterType::PERC_CACHE_FORWARDS && cluster->type != ClusterType::PERC_CACHE_REVERSED) {
		display->freezeWithError("E149");
	}
	if (!percCacheClusters[reversed]) {
		display->freezeWithError("E134");
	}
	if (cluster->clusterIndex >= numPercCacheClusters) {
		display->freezeWithError("E135");
	}
	if (!percCacheClusters[reversed][cluster->clusterIndex]) {
		display->freezeWithError("i034"); // Trying to track down Steven G's E133 (Feb 2021).
	}
	if (percCacheClusters[reversed][cluster->clusterIndex]->numReasonsToBeLoaded) {
		display->freezeWithError("i035"); // Trying to track down Steven G's E133 (Feb 2021).
	}
#endif

	percCacheClusters[reversed][cluster->clusterIndex] = NULL;

	// TODO: while inside this, don't allow further editing to percCacheZones[reversed]

	int32_t leftBorder = cluster->clusterIndex
	                     << (audioFileManager.clusterSizeMagnitude + kPercBufferReductionMagnitude);
	int32_t rightBorder = (cluster->clusterIndex + 1)
	                      << (audioFileManager.clusterSizeMagnitude + kPercBufferReductionMagnitude);

	int32_t laterBorder = reversed ? (leftBorder - 1) : rightBorder;
	int32_t earlierBorder = reversed ? (rightBorder - 1) : leftBorder;

	// Trim anything earlier
	int32_t iEarlier;
	iEarlier = percCacheZones[reversed].search(earlierBorder + reversed, comparison);
	if (iEarlier >= 0 && iEarlier < percCacheZones[reversed].getNumElements()) {
		SamplePercCacheZone* zoneEarlier = (SamplePercCacheZone*)percCacheZones[reversed].getElementAddress(iEarlier);

		// If this zone eats into the deleted Cluster...
		if ((zoneEarlier->endPos - earlierBorder) * playDirection > 0) {

			// If it also shoots out the other side of the deleted Cluster...
			if ((zoneEarlier->endPos - laterBorder) * playDirection > 0) {
				int32_t oldStartPos = zoneEarlier->startPos;
				int32_t oldSamplesAtStartWhichShouldBeReplaced = zoneEarlier->samplesAtStartWhichShouldBeReplaced;

				zoneEarlier->startPos = laterBorder;
				zoneEarlier->samplesAtStartWhichShouldBeReplaced = 0;

				int32_t iNew = reversed ? (iEarlier + 1) : iEarlier;
				// This is reasonably likely to fail, cos it might want to allocate new memory, but that's not allowed if it's currently allocating a Cluster, which it will
				// be if this Cluster got stolen, which is why we're here. Oh well
				int32_t error = percCacheZones[reversed].insertAtIndex(
				    iNew, 1,
				    this); // Also specify not to steal perc cache Clusters from this Sample. Could that actually even happen given the above comment? Not sure.
				if (error) {
					Debug::println("insert fail");
					LOCK_EXIT
					return;
				}

				SamplePercCacheZone* newZone =
				    new (percCacheZones[reversed].getElementAddress(iNew)) SamplePercCacheZone(oldStartPos);
				newZone->samplesAtStartWhichShouldBeReplaced = oldSamplesAtStartWhichShouldBeReplaced;
				newZone->endPos = earlierBorder;
				LOCK_EXIT
				return;
			}

			// Or if not...
			else {
				zoneEarlier->resetEndPos(earlierBorder);
			}
		}
	}

	// Trim anything later
	int32_t iLater;
	iLater = percCacheZones[reversed].search(laterBorder + reversed, comparison);
	if ((iLater - iEarlier) * playDirection > 0) {

		SamplePercCacheZone* zoneLater = (SamplePercCacheZone*)percCacheZones[reversed].getElementAddress(iLater);

		if ((zoneLater->endPos - laterBorder) * playDirection > 0) {
			zoneLater->samplesAtStartWhichShouldBeReplaced =
			    std::max(0_i32, zoneLater->samplesAtStartWhichShouldBeReplaced
			                        - (laterBorder - zoneLater->startPos) * playDirection);
			zoneLater->startPos = laterBorder;
		}
		else {
			goto deleteThatOneToo;
		}
	}
	else {
deleteThatOneToo:
		iLater += playDirection;
	}

	int32_t numToDelete = (iLater - iEarlier) * playDirection - 1;
	if (numToDelete) {
		int32_t deleteFrom = reversed ? (iLater + 1) : (iEarlier + 1);
		percCacheZones[reversed].deleteAtIndex(deleteFrom, numToDelete);
	}

	LOCK_EXIT
}

int32_t Sample::getFirstClusterIndexWithAudioData() {
	return audioDataStartPosBytes >> audioFileManager.clusterSizeMagnitude;
}

int32_t Sample::getFirstClusterIndexWithNoAudioData() {
	uint32_t clusterIndex =
	    ((audioDataStartPosBytes + audioDataLengthBytes - 1) >> audioFileManager.clusterSizeMagnitude) + 1; // Rounds up
	if (clusterIndex > clusters.getNumElements()) {
		clusterIndex = clusters.getNumElements();
	}
	return clusterIndex;
}

void Sample::workOutMIDINote(bool doingSingleCycle, float minFreqHz, float maxFreqHz, bool doPrimeTest) {
	if (midiNote == MIDI_NOTE_UNSET || midiNote == MIDI_NOTE_ERROR) {

		float freq;

		// If doing single-cycle, easy!
		if (doingSingleCycle) {
			freq = (float)sampleRate / lengthInSamples;
			goto calculateMIDINote;
		}

		// Next up, see if note read from file...
		else if (midiNoteFromFile != -1) {
			midiNote = midiNoteFromFile;
		}

		// And finally, detect the pitch the hard way
		else {
			freq = determinePitch(doingSingleCycle, minFreqHz, maxFreqHz, doPrimeTest);

			if (freq == 0) { // Error
				midiNote = MIDI_NOTE_ERROR;
			}

			else {
calculateMIDINote:
				midiNote = 69 + log2f(freq / 440) * 12;
			}
		}
	}

	//Debug::print("midiNote: ");
	//Debug::printlnfloat(midiNote);
}

uint32_t Sample::getLengthInMSec() {
	return (uint64_t)(lengthInSamples - 1) * 1000 / sampleRate + 1;
}

float getPeakIndexFloat(int32_t i, int32_t peakValue, int32_t prevValue, int32_t nextValue) {
	float fundamentalPeakIndex = i;

	int32_t nudgeInDirection = (nextValue > prevValue) ? 1 : -1;

	int32_t lowerValue = std::min(prevValue, nextValue);
	int32_t higherValue = std::max(prevValue, nextValue);

	int32_t totalDistance = peakValue - lowerValue; // Distance from lower neighbouring height to peak height

	int32_t howFarUpHigherValueIs = higherValue - lowerValue;

	float howFarAsFraction = (float)howFarUpHigherValueIs / totalDistance;

	fundamentalPeakIndex += howFarAsFraction * 0.5 * nudgeInDirection;

	return fundamentalPeakIndex;
}

const uint8_t primeNumbers[] = {2, 3, 5, 7, 11, 13};
constexpr int32_t kNumPrimes = 6;

// Returns strength
int32_t Sample::investigateFundamentalPitch(int32_t fundamentalIndexProvided, int32_t tableSize, int32_t* heightTable,
                                            uint64_t* sumTable, float* floatIndexTable, float* getFundamentalIndex,
                                            int32_t numDoublings, bool doPrimeTest) {

	uint64_t total = 0;

	uint64_t primeTotals[kNumPrimes];
	if (true || doPrimeTest) {
		memset(primeTotals, 0, sizeof(primeTotals));
	}

	float uncertaintyCount = 1.5;
	float fundamentalIndexToReturn;
	float fundamentalIndexForContinuedHarmonicInvestigation;
	float uncertaintyMarginHere;

	int32_t currentIndex = fundamentalIndexProvided;
	int32_t h = 1; // The number of the harmonic currently being investigated
	int32_t lastHFound = 1;

	uint64_t lastSumTableValue = sumTable[fundamentalIndexProvided >> 1];

	goto examineHarmonic;

	while (true) {

		{
			if (uncertaintyCount >= 10.5) {
				break; // Probably not really necessary
			}

			if (h == 16) {
				break; // Limit number of harmonics investigated
			}
			h++;

			uncertaintyMarginHere = uncertaintyCount;

			if (uncertaintyMarginHere < 2) {
				uncertaintyMarginHere = 2;
			}

			if (uncertaintyMarginHere > (fundamentalIndexProvided >> 1)) {
				uncertaintyMarginHere = (fundamentalIndexProvided >> 1);
			}

			float searchCentre =
			    fundamentalIndexForContinuedHarmonicInvestigation * h + 0.5; // Will round when converted to int32_t

			int32_t searchMax = searchCentre + uncertaintyMarginHere;
			if (searchMax >= tableSize) {
				break;
			}
			int32_t searchMin = searchCentre - uncertaintyMarginHere;

			int32_t highestFoundHere = 0;

			for (int32_t proposedIndex = searchMin; proposedIndex <= searchMax; proposedIndex++) {
				int32_t valueHere = heightTable[proposedIndex];
				if (valueHere > highestFoundHere) {
					highestFoundHere = valueHere;
					currentIndex = proposedIndex;
				}
			}

			uncertaintyCount += (float)1.5 / lastHFound;

			if (!highestFoundHere) {
				continue;
			}
		}

examineHarmonic:
		float newEstimatedFundamentalIndex = floatIndexTable[currentIndex >> 1] / h;

		int32_t nextMidIndex = currentIndex + ((fundamentalIndexProvided + 1) >> 1); // Round up
		uint64_t nextSumTableValue = sumTable[nextMidIndex];
		uint64_t surroundingSum = nextSumTableValue - lastSumTableValue;

		lastSumTableValue = nextSumTableValue;

		int32_t heightRightHere = heightTable[currentIndex];
		int32_t heightRelativeToSurroundings = ((uint64_t)heightRightHere << 18) / surroundingSum;

		int32_t strengthThisHarmonic = ((uint64_t)heightRelativeToSurroundings * (uint64_t)heightRightHere) >> 20;
		total += strengthThisHarmonic;

		if (h == 1) {
			fundamentalIndexForContinuedHarmonicInvestigation = newEstimatedFundamentalIndex;
		}

		else {
			float distanceToGo = newEstimatedFundamentalIndex - fundamentalIndexForContinuedHarmonicInvestigation;
			float heightRelativeToSurroundingsFloat = (float)heightRelativeToSurroundings / (1 << 18);
			if (heightRelativeToSurroundingsFloat > 1) {
				heightRelativeToSurroundingsFloat = 1;
			}
			fundamentalIndexForContinuedHarmonicInvestigation += distanceToGo * heightRelativeToSurroundingsFloat;

			float uncertaintyReduction = heightRelativeToSurroundingsFloat * 8;
			if (uncertaintyReduction < 1) {
				uncertaintyReduction = 1;
			}

			uncertaintyCount /= uncertaintyReduction;
			if (uncertaintyCount < 1.5) {
				uncertaintyCount = 1.5;
			}
		}

		if (true || doPrimeTest) {
			for (int32_t p = 0; p < kNumPrimes; p++) {
				if (p == 0 && !doPrimeTest) {
					continue;
				}

				uint8_t thisPrime = primeNumbers[p];
				if (thisPrime > h) {
					break;
				}

				if (!((uint32_t)h % thisPrime)) {
					primeTotals[p] += strengthThisHarmonic;
				}
			}
		}

		// After working far enough into the table, we want to stop adjusting the pitch we're going to output, because the higher harmonics tend to be a bit sharp, at least initially, on a lot of acoustic instruments.
		if (h == 1 || currentIndex < 128) {
			fundamentalIndexToReturn = fundamentalIndexForContinuedHarmonicInvestigation;
		}

		lastHFound = h;

#if PITCH_DETECT_DEBUG_LEVEL >= 2
		Debug::print("found harmonic ");
		Debug::print(h);
		Debug::print(". value ");
		Debug::print(heightTable[currentIndex]);
		Debug::print(", ");
		Debug::print((heightRelativeToSurroundings * 100) >> 18);
		float fundamentalPeriod = (float)PITCH_DETECT_WINDOW_SIZE / fundamentalIndexForContinuedHarmonicInvestigation;
		float freqBeforeAdjustment = (float)sampleRate / fundamentalPeriod;
		float freq = freqBeforeAdjustment / (1 << numDoublings);
		Debug::print("%. proposed freq: ");
		Debug::printfloat(freq);
		Debug::print(". uc: ");
		Debug::printlnfloat(uncertaintyCount);
		delayMS(30);
#endif
	}

	*getFundamentalIndex = fundamentalIndexToReturn;

	int32_t threshold = 6;

	if (true || doPrimeTest) {
		for (int32_t p = 0; p < kNumPrimes; p++) {
			uint8_t thisPrime = primeNumbers[p];
			if (thisPrime > h) {
				break;
			}

			//if (primeTotals[p] >= (total - primeTotals[p]) / (thisPrime - 1) * threshold) return 0;
			if (primeTotals[p] * (thisPrime - 1) >= (total - primeTotals[p]) * threshold) {
				//Debug::println("failing due to prime thing");
				return 0;
			}
		}
	}

	// Too low and piano doesn't work. Too high and vibraphone doesn't work

	// With FFT m=12
	// No delay: doesn't work
	// 1/8 second delay: 0.35 to 0.40
	// 1/4 second delay: 0.25 to 0.55
	// 1/2 second delay: 0.65 is max for vibraphone. Never quite get all piano working

	// With FFT m=13
	// 1/4 second delay: -0.05 to 0.55

	return (uint64_t)(total * powf(fundamentalIndexToReturn, 0.25));
}

// In Hz I think? Could even go +2 here and even a 54Hz sound is ok
constexpr int32_t kMinAccurateFrequency = (1638400 >> (kPitchDetectWindowSizeMagnitude));
constexpr int32_t kMaxLengthDoublings = (16 - kPitchDetectWindowSizeMagnitude);

// We want a fairly small window. Any bigger, and it'll fail to find the tones in short, percussive yet tonal sounds.
// Or if we were to go much smaller than this, we might incorrectly see low frequencies.
// Already, this is too small to very accurately pick up low frequencies, so when one is detected, a second pass is done on downsampled (squished in) audio data, to pick it up more accurately

// Returns 0 if error
float Sample::determinePitch(bool doingSingleCycle, float minFreqHz, float maxFreqHz, bool doPrimeTest) {

#if PITCH_DETECT_DEBUG_LEVEL
	delayMS(200);
	Debug::println("");
	Debug::println("det. pitch --");
	Debug::println(filePath.get());
#endif

	// Get the FFT config we'll need
	ne10_fft_r2c_cfg_int32_t fftCFG = FFTConfigManager::getConfig(kPitchDetectWindowSizeMagnitude);

	// Allocate space for both the real and imaginary number buffers - the imaginary one is tacked on the end
	int32_t fftInputSize = kPitchDetectWindowSize * sizeof(int32_t);
	int32_t fftOutputSize = ((kPitchDetectWindowSize >> 1) + 1) * sizeof(ne10_fft_cpx_int32_t);
	int32_t floatIndexTableSize = (kPitchDetectWindowSize >> 2) * sizeof(float);
	int32_t* fftInput = (int32_t*)GeneralMemoryAllocator::get().alloc(
	    fftInputSize + fftOutputSize + floatIndexTableSize, NULL, false, true);
	if (!fftInput) {
		return 0;
	}

	ne10_fft_cpx_int32_t* fftOutput = (ne10_fft_cpx_int32_t*)((uint32_t)fftInput + fftInputSize);
	int32_t* fftHeights = fftInput; // We'll overwrite the original input with this data

	float* floatIndexTable = (float*)((uint32_t)fftInput + fftInputSize + fftOutputSize);

	int32_t defaultLengthDoublings = 0;

	// If high sample rate, downsample by default
	if (sampleRate >= 88200) {
		defaultLengthDoublings++;
	}

	int32_t lengthDoublings = defaultLengthDoublings;

	// If enforced max freq too low, increase doublings
	float maxFreqHere = maxFreqHz;
	while (maxFreqHere < kMinAccurateFrequency) {
		lengthDoublings++;
		if (lengthDoublings >= 10) {
			return 0; // Keep things sane / from overflowing, which I saw happen when lengthDoublings got to 15. Happened when another error led to maxFreq being insanely low like almost 0
		}
		maxFreqHere *= 2;
	}

	bool doingSecondPassWithReducedThreshold = false;
	int32_t startValueThreshold = 1 << (31 - 4);
	if (!beginningOffsetForPitchDetection) {
		beginningOffsetForPitchDetection = audioDataStartPosBytes;
	}

startAgain:

#if PITCH_DETECT_DEBUG_LEVEL
	Debug::println("");
	Debug::print("doublings: ");
	Debug::println(lengthDoublings);
#endif

	// Load the sample into memory
	int32_t currentOffset = beginningOffsetForPitchDetection;
	uint32_t currentClusterIndex = currentOffset >> audioFileManager.clusterSizeMagnitude;
	int32_t writeIndex = 0;

	Cluster* cluster =
	    clusters.getElement(currentClusterIndex)->getCluster(this, currentClusterIndex, CLUSTER_LOAD_IMMEDIATELY);
	if (!cluster) {
		Debug::println("failed to load first");
getOut:
		delugeDealloc(fftInput);
		return 0;
	}

	Cluster* nextCluster = NULL;

	int32_t biggestValueFound = 0;

	int32_t count = 0;

	// If stereo sample, we want to blend left and right together, and the easiest way is to use our existing "averaging" system
	int32_t lengthDoublingsNow = lengthDoublings;
	if (numChannels == 2) {
		lengthDoublingsNow++;
	}

	while (true) {
continueWhileLoop:
		// If there's no "next" Cluster, load it now
		if (!nextCluster && currentClusterIndex + 1 < getFirstClusterIndexWithNoAudioData()) {
			nextCluster = clusters.getElement(currentClusterIndex + 1)
			                  ->getCluster(this, currentClusterIndex + 1, CLUSTER_LOAD_IMMEDIATELY);
			if (!nextCluster) {
				audioFileManager.removeReasonFromCluster(cluster, "imcwn4o");
				Debug::println("failed to load next");
				goto getOut;
			}
		}

		int32_t thisValue = 0;

		// We may want to average several samples into just one - crudely downsampling, but the aliasing shouldn't hurt us
		for (int32_t i = 0; i < (1 << lengthDoublingsNow); i++) {

			if (!(count & 255)) {
				AudioEngine::routineWithClusterLoading(); // --------------------------------------
			}
			count++;

			int32_t individualSampleValue =
			    *(int32_t*)&cluster->data[(currentOffset & (audioFileManager.clusterSize - 1)) - 4 + byteDepth]
			    & bitMask;
			thisValue += (individualSampleValue >> lengthDoublingsNow);

			currentOffset += byteDepth;

			// If reached end of file
			if (currentOffset >= audioDataLengthBytes + audioDataStartPosBytes) {
				goto doneReading;
			}

			uint32_t newClusterIndex = currentOffset >> audioFileManager.clusterSizeMagnitude;

			// If passed Cluster end...
			if (newClusterIndex != currentClusterIndex) {
				currentClusterIndex = newClusterIndex;

				audioFileManager.removeReasonFromCluster(cluster, "hset");
				cluster = nextCluster;
				nextCluster = NULL; // It'll soon get filled
			}

			// Rudimentary audio start-detection. We need this, because detecting the tone of percussive sounds relies on having our window at just the moment when they hit
			if (!beginningOffsetForPitchDetectionFound) {
				int32_t absoluteValue = (individualSampleValue < 0) ? -individualSampleValue : individualSampleValue;

				if (absoluteValue > biggestValueFound) {
					biggestValueFound = absoluteValue;
				}

				if (absoluteValue < startValueThreshold) {
					goto continueWhileLoop;
				}
				beginningOffsetForPitchDetectionFound = true;

				// Start grabbing audio from a quarter of a second after here
				beginningOffsetForPitchDetection =
				    currentOffset + (sampleRate >> 2) * numChannels * byteDepth; // Save it for next time

				// If our grabbed window would end beyond the end of the audio file, shift it left
				beginningOffsetForPitchDetection =
				    std::min(beginningOffsetForPitchDetection,
				             (int32_t)(audioDataStartPosBytes + audioDataLengthBytes
				                       - (kPitchDetectWindowSize << lengthDoublings) * numChannels * byteDepth));

				// TODO: it's not quite perfect doing that and storing the result, because lengthDoublings will sometimes be different

				// And now make sure that hasn't pushed it further back left than where we are right now
				beginningOffsetForPitchDetection = std::max(beginningOffsetForPitchDetection, currentOffset);
			}
			if (currentOffset < beginningOffsetForPitchDetection) {
				goto continueWhileLoop;
			}
		}

		// Do hanning window
		int32_t hanningValue = interpolateTableSigned(writeIndex, kPitchDetectWindowSizeMagnitude, hanningWindow, 8);

		fftInput[writeIndex] = multiply_32x32_rshift32_rounded(thisValue, hanningValue) >> 12;

		writeIndex++;

		if (writeIndex >= kPitchDetectWindowSize) {
			break;
		}
	}

doneReading:
	audioFileManager.removeReasonFromCluster(cluster, "kncd");
	if (nextCluster) {
		audioFileManager.removeReasonFromCluster(nextCluster, "ljpp");
	}

	// If we didn't find any sound...
	if (!beginningOffsetForPitchDetectionFound) {

		// If we haven't done so yet, see if we can just go again, with a reduced threshold derived from the actual volume of the sound
		if (!doingSecondPassWithReducedThreshold && biggestValueFound >= (1 << (31 - 9))) {
			doingSecondPassWithReducedThreshold = true;
			startValueThreshold = biggestValueFound >> 4;
			goto startAgain;
		}

		Debug::println("no sound found");
		goto getOut;
	}

	// If there was any space left...
	while (writeIndex < kPitchDetectWindowSize) {
		fftInput[writeIndex] = 0;
		writeIndex++;
	}

	AudioEngine::routineWithClusterLoading(); // --------------------------------------------------

	/*
	Debug::print("doing fft ----------------");
	Debug::println(PITCH_DETECT_WINDOW_SIZE_MAGNITUDE);
	uint16_t startTime = MTU2.TCNT_0;
	*/

	// Perform the FFT
	ne10_fft_r2c_1d_int32_neon(fftOutput, (ne10_int32_t*)fftInput, fftCFG, false);

	//Debug::println("fft done");

	/*
	uint16_t endTime = MTU2.TCNT_0;
	uint16_t time = endTime - startTime;
	Debug::print("fft time uSec: ");
	Debug::println(timerCountToUS(time));
	*/
	AudioEngine::logAction("bypassing culling in pitch detection");
	AudioEngine::bypassCulling = true;
	AudioEngine::routineWithClusterLoading();

	// Go through complex-number FFT result, converting to positive (pythagorassed) heights
	int32_t biggestValue = 0;
	for (int32_t i = 0; i < (kPitchDetectWindowSize >> 1); i++) {

		if (!(i & 1023)) {
			AudioEngine::routineWithClusterLoading(); // --------------------------------------
		}

		int32_t thisValue = fastPythag(fftOutput[i].r, fftOutput[i].i);
		if (thisValue > biggestValue) {
			biggestValue = thisValue;
		}

		fftHeights[i] = thisValue;

#if PITCH_DETECT_DEBUG_LEVEL >= 2
		if (!(i & 31)) {
			Debug::println("");
			Debug::print(i);
			Debug::print(": ");
			delayMS(50);
		}
		Debug::print(thisValue);
		Debug::print(", ");
#endif
	}

	int32_t minFreqForThresholdAdjusted = 200 << lengthDoublings;
	float minPeriodForThreshold = sampleRate / minFreqForThresholdAdjusted;
	int32_t minIndexForThreshold = (float)kPitchDetectWindowSize / minPeriodForThreshold; // Rounds down

	uint64_t sum = 0;
	int32_t lastValue1;
	int32_t lastValue2;
	int32_t threshold = biggestValue >> 10;

	// Go through again doing the running sum, interpolating exact peak frequencies, and deleting everything that's not a peak
	for (int32_t i = 0; i < (kPitchDetectWindowSize >> 1); i++) {

		if (!(i & 255)) {
			AudioEngine::routineWithClusterLoading(); // --------------------------------------
		}

		int32_t thisValue = fftHeights[i];

		// Don't bother with anything under the threshold - mostly just for efficiency, since the threshold is very low and
		// won't cause much real-world difference.
		// Don't do it below a certain freq though - we absolutely need even the tiniest peaks down in the 30hz kind of range (see Leo's pianos)
		bool shouldWriteZeroBack = (i >= minIndexForThreshold && lastValue1 < threshold);
		if (!shouldWriteZeroBack) {
			bool isPeakHere = (i >= 2 && thisValue < lastValue1 && lastValue1 >= lastValue2);

			if (isPeakHere) {
				floatIndexTable[(i - 1) >> 1] = getPeakIndexFloat(i - 1, lastValue1, lastValue2, thisValue);
			}

			shouldWriteZeroBack = !isPeakHere;
		}

		if (i >= 1 && shouldWriteZeroBack) {
			fftHeights[i - 1] = 0;
		}

		sum += lastValue1;
		*(uint64_t*)&fftOutput[i] = sum;

		lastValue2 = lastValue1;
		lastValue1 = thisValue;
	}

#if PITCH_DETECT_DEBUG_LEVEL
	Debug::println("");
#endif

	int32_t minFreqAdjusted = minFreqHz * (1 << lengthDoublings);
	float minFundamentalPeriod = (float)sampleRate / minFreqAdjusted;
	int32_t minFundamentalPeakIndex = (float)kPitchDetectWindowSize / minFundamentalPeriod; // Rounds down

	int32_t maxFreqAdjusted = maxFreqHz * (1 << lengthDoublings);
	float maxFundamentalPeriod = (float)sampleRate / maxFreqAdjusted;
	int32_t maxFundamentalPeakIndex = (float)kPitchDetectWindowSize / maxFundamentalPeriod + 1; // Rounds up
	if (maxFundamentalPeakIndex > (kPitchDetectWindowSize >> 1)) {
		maxFundamentalPeakIndex = (kPitchDetectWindowSize >> 1);
	}

	float bestFundamentalIndex;
	int32_t bestStrength = 0;

	int32_t peakCount = 0;

	// For each peak, evaluate its strength as a contender for the fundamental
	for (int32_t i = minFundamentalPeakIndex; i < maxFundamentalPeakIndex; i++) {

		if (!fftHeights[i]) {
			continue;
		}

		// We're at a peak!

		if (!(peakCount & 7)) {
			AudioEngine::
			    routineWithClusterLoading(); // -------------------------------------- // 15 works. 7 is extra safe
		}
		peakCount++;

		float fundamentalIndexHere;
		int32_t strengthHere =
		    investigateFundamentalPitch(i, (kPitchDetectWindowSize >> 1), fftHeights, (uint64_t*)fftOutput,
		                                floatIndexTable, &fundamentalIndexHere, lengthDoublings, doPrimeTest);

#if PITCH_DETECT_DEBUG_LEVEL
		if (PITCH_DETECT_DEBUG_LEVEL >= 2 || strengthHere > bestStrength) {
			delayMS(10);

			float fundamentalPeriod = (float)PITCH_DETECT_WINDOW_SIZE / fundamentalIndexHere;
			float freqBeforeAdjustment = (float)sampleRate / fundamentalPeriod;
			float freq = freqBeforeAdjustment / (1 << lengthDoublings);

			Debug::print("strength ");
			Debug::print(strengthHere);
			//Debug::print(" at i ");
			//Debug::print(i);
			Debug::print(", freq ");
			Debug::printlnfloat(freq);
#if PITCH_DETECT_DEBUG_LEVEL >= 2
			Debug::println("");
#endif
		}
#endif

		if (strengthHere > bestStrength) {

			bestStrength = strengthHere;
			bestFundamentalIndex = fundamentalIndexHere;
		}
	}

	// If no peaks found, print out the FFT for debugging
	if (!bestStrength) {
		Debug::println("no peaks found.");

		Debug::print("searching ");
		Debug::print(minFundamentalPeakIndex);
		Debug::print(" to ");
		Debug::println(maxFundamentalPeakIndex);

#if PITCH_DETECT_DEBUG_LEVEL
		for (int32_t i = 0; i < (PITCH_DETECT_WINDOW_SIZE >> 1); i++) {
			if (!(i & 31)) {
				Debug::println("");
				Debug::print(i);
				Debug::print(": ");
				delayMS(50);
			}
			Debug::print(fftHeights[i]);
			Debug::print(", ");
		}
#endif
		goto getOut;
	}

	float fundamentalPeriod = (float)kPitchDetectWindowSize / bestFundamentalIndex;

	float freqBeforeAdjustment = (float)sampleRate / fundamentalPeriod;

	int32_t lengthDoublingsLastTime = lengthDoublings;

	// If frequency too low, go again, taking a longer length into account, for better accuracy
	if (freqBeforeAdjustment < kMinAccurateFrequency
	    && lengthDoublings < defaultLengthDoublings + kMaxLengthDoublings) {

#if PITCH_DETECT_DEBUG_LEVEL
		float freq = freqBeforeAdjustment / (1 << lengthDoublings);
		Debug::print("proposed freq: ");
		Debug::printlnfloat(freq);
#endif
		// Only do one doubling at a time - this can help to correct an incorrect reading
		freqBeforeAdjustment *= 2;
		lengthDoublings++;

		goto startAgain;
	}

	delugeDealloc(fftInput);

	float freq = freqBeforeAdjustment / (1 << lengthDoublings);
	Debug::print("freq: ");
	uartPrintlnFloat(freq);

	return freq;
}

void Sample::convertDataOnAnyClustersIfNecessary() {
	if (rawDataFormat) {
		for (int32_t c = getFirstClusterIndexWithAudioData(); c < getFirstClusterIndexWithNoAudioData(); c++) {
			Cluster* cluster = clusters.getElement(c)->cluster;
			if (cluster) {

				// Add reason in case it would get stolen
				audioFileManager.addReasonToCluster(cluster);

				cluster->convertDataIfNecessary();

				audioFileManager.removeReasonFromCluster(cluster, "E231");
			}
		}
	}
}

int32_t Sample::getMaxPeakFromZero() {
	// Comes out one >> of the value we actually want
	int32_t halfValue = std::abs(getFoundValueCentrePoint() >> 1) + (maxValueFound >> 2) - (minValueFound >> 2);

	// Does the <<1 and saturates it - this was necessary, it was overflowing sometimes - I think when the audio clipped
	return lshiftAndSaturate<1>(halfValue);
}

int32_t Sample::getFoundValueCentrePoint() {
	return (maxValueFound >> 1) + (minValueFound >> 1);
}

// Returns the value span divided by display height
int32_t Sample::getValueSpan() {
	return (maxValueFound >> kDisplayHeightMagnitude) - (minValueFound >> kDisplayHeightMagnitude);
}

void Sample::finalizeAfterLoad(uint32_t fileSize) {

	audioDataLengthBytes = std::min<uint64_t>(audioDataLengthBytes, fileSize - audioDataStartPosBytes);

	// If floating point file, Clusers can only be float-processed (as they're loaded) once we've found the data start-pos, which we just did, and
	// since we've already loaded that first cluster which contains data, we'd better float-process it now!
	convertDataOnAnyClustersIfNecessary();

	uint32_t bytesPerSample = byteDepth * numChannels;

	audioDataLengthBytes = std::min<uint64_t>(audioDataLengthBytes, fileSize - audioDataStartPosBytes);

	lengthInSamples = audioDataLengthBytes / bytesPerSample;
	audioDataLengthBytes = lengthInSamples * bytesPerSample; // Make sure it's an exact number of samples

	workOutBitMask();
}

#if ALPHA_OR_BETA_VERSION
void Sample::numReasonsDecreasedToZero(char const* errorCode) {

	// Count up the individual reasons, as a bug check
	int32_t numClusterReasons = 0;
	for (int32_t c = 0; c < clusters.getNumElements(); c++) {

		Cluster* cluster = clusters.getElement(c)->cluster;
		if (cluster) {

			if (cluster->clusterIndex != c) {
				// Leo got! Aug 2020. Suspect some sort of memory corruption... And then Michael got, Feb 2021
				display->freezeWithError(errorCode);
			}

			if (cluster->numReasonsToBeLoaded < 0) {
				display->freezeWithError("E076");
			}

			numClusterReasons += cluster->numReasonsToBeLoaded;

			if (cluster == audioFileManager.clusterBeingLoaded) {
				numClusterReasons--;
			}
		}
		//clusters[c].ensureNoReason(this);
	}

	if (numClusterReasons) {
		Debug::println("reason dump---");
		for (int32_t c = 0; c < clusters.getNumElements(); c++) {

			Cluster* cluster = clusters.getElement(c)->cluster;
			if (cluster) {
				Debug::print(cluster->numReasonsToBeLoaded);

				if (cluster == audioFileManager.clusterBeingLoaded) {
					Debug::println(" (loading)");
				}
				else if (!cluster->loaded) {
					Debug::println(" (unloaded)");
				}
				else {
					Debug::println("");
				}
			}
			else {
				Debug::println("*");
			}
		}
		Debug::println("/reason dump---");

		display->freezeWithError(
		    "E078"); // LegsMechanical got, V4.0.0-beta2. https://forums.synthstrom.com/discussion/4106/v4-0-beta2-e078-crash-when-recording-audio-clip
	}
}
#endif
