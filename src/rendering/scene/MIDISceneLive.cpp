#include <stdio.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>

#include "../../helpers/ProgramUtilities.h"
#include "../../helpers/ResourcesManager.h"
#include "../../midi/MIDIUtils.h"

#include "MIDISceneLive.h"

#include <libremidi/writer.hpp>


#ifdef _WIN32
#undef MIN
#undef MAX
#endif

#define MAX_NOTES_IN_FLIGHT 8192
// A live note recording longer than this (seconds) is treated as stuck (its
// note-off was lost) and force-closed. Real piano notes never sustain this long.
#define STUCK_NOTE_TIMEOUT 30.0
// A frame longer than this (seconds) means the render loop stalled (app
// backgrounded / AFK), not real playback. Used to auto-close recording notes so
// they don't stretch their duration across the whole idle gap.
#define LIVE_STALL_GAP 0.5
// Upper bounds to keep memory from growing without limit during a very long
// live session (these only trim ancient history, never anything still on screen).
#define MAX_STORED_FRAMES 500000
#define MAX_STORED_PEDAL_EVENTS 8192

MIDISceneLive::~MIDISceneLive(){
	shared().close_port();
}

MIDISceneLive::MIDISceneLive(int port, bool verbose) : MIDIScene() {
	_verbose = verbose;
	
	// For now we use the same MIDI in instance for everything.
	if(shared().is_port_open()){
		shared().close_port();
	}
	if(port >= 0){
		shared().open_port(port, "MIDIVisualizer input");
		_deviceName = _availablePorts[port];
	} else {
		shared().open_virtual_port("MIDIVisualizer virtual input");
		_deviceName = VIRTUAL_DEVICE_NAME;
	}
	shared().ignore_types(true, true, true);

	_activeIds.fill(-1);
	_activeRecording.fill(false);
	_rawToDisplay.fill(-1);
	_notes.resize(MAX_NOTES_IN_FLIGHT);
	_notesInfos.resize(MAX_NOTES_IN_FLIGHT);
	_noteStartTimes.assign(MAX_NOTES_IN_FLIGHT, 0.0);
	_allMessages.reserve(MAX_NOTES_IN_FLIGHT);
	_secondsPerMeasure = computeMeasureDuration(_tempo, _signatureNum / _signatureDenom);
	_pedalInfos[-10000.0] = Pedals();

	_dirtyNotes = true;
	_dirtyNotesRange = {0, 0}; // Full array
}

void MIDISceneLive::updateSetsAndVisibleNotes(const SetOptions & options, const FilterOptions& filter){
	_currentSetOption = options;
	
	// _notesCount keeps growing past the ring buffer size, only the stored notes can be updated.
	const size_t storedCount = size_t((std::min)(_notesCount, MAX_NOTES_IN_FLIGHT));
	for(size_t nid = 0; nid < storedCount; ++nid){
		auto & note = _notes[nid];
		// Restore key and channel from note infos (note.note is the rendering index, not the key).
		const int set = _currentSetOption.apply(int(_notesInfos[nid].note), _notesInfos[nid].channel, 0, note.start);
		note.set = float(set);
	}

	updateVisibleNotes(filter);
}

void MIDISceneLive::updateVisibleNotes(const FilterOptions& filter){
	// Don't apply filter on live scenes. Assume the user won't want to hide what they are recording.
	(void)filter;

	// Just trigger a refresh of the full array.
	_dirtyNotes = true;
	_dirtyNotesRange = {0, 0};
}

void MIDISceneLive::updatesActiveNotes(double time, double speed, const FilterOptions& filter){
	// Don't apply filter on live scenes. Assume the user won't want to hide what they are recording.
	(void)filter;

	int minUpdated = MAX_NOTES_IN_FLIGHT;
	int maxUpdated = 0;

	// If we are paused, drain the queue but still honour note-offs so that a note
	// released while paused doesn't stay "recording" (and stuck) once we resume.
	if(_previousTime == time){
		while(true){
			auto message = shared().get_message();
			if(message.size() == 0){
				// End of the queue.
				break;
			}
			if(message.is_note_on_or_off()){
				const bool isOff = (message.get_message_type() != libremidi::message_type::NOTE_ON) || (clamp<short>(short(message[2]), 0, 127) == 0);
				if(isOff){
					releaseRawKey(clamp<short>(short(message[1]), 0, 127));
				}
			}
		}
		return;
	}

	// If the render loop was stalled (app backgrounded / AFK), the frame gap is huge
	// and any note still "recording" would otherwise absorb the whole gap into its
	// duration (a giant stuck note). Detect the gap and close recording notes at the
	// last good frame time instead, so their real note-offs (queued during the stall)
	// become harmless no-ops and notes played after resume start fresh.
	const double dt = time - _previousTime;
	if(dt > LIVE_STALL_GAP){
		for(size_t nid = 0; nid < _activeRecording.size(); ++nid){
			if(!_activeRecording[nid]){
				continue;
			}
			const int noteId = _activeIds[nid];
			if(noteId >= 0){
				// Cap the duration at the last good frame time, not the resumed time.
				const double heldFor = _previousTime - _noteStartTimes[noteId];
				_notes[noteId].duration = (std::max)(float(heldFor), 0.0f);
			}
			_activeRecording[nid] = false;
			_actives[nid] = -1;
			_activeIds[nid] = -1;
		}
		_rawToDisplay.fill(-1);
		_dirtyNotes = true;
		_dirtyNotesRange = {0, 0}; // full refresh; a stall is rare
	}

	// Update the particle systems lifetimes.
	for(auto & particle : _particles){
		// Give a bit of a head start to the animation.
		particle.elapsed = (float(time) - particle.start + 0.25f) / (float(speed) * particle.duration);
		// Disable particles that shouldn't be visible at the current time.
		if(float(time) >= particle.start + particle.duration || float(time) < particle.start){
			particle.note = -1;
			particle.set = -1;
			particle.duration = particle.start = particle.elapsed = 0.0f;
		}
	}

	// Restore all active flags.
	for(size_t nid = 0; nid < _actives.size(); ++nid){
		_actives[nid] = -1;
	}

	// Update all currently recording notes, extending their duration.
	for(size_t nid = 0; nid < _actives.size(); ++nid){
		if(!_activeRecording[nid]){
			continue;
		}
		const int noteId = _activeIds[nid];
		GPUNote & note = _notes[noteId];
		// Use the double-precision start so the duration stays exact after long uptime.
		const double heldFor = time - _noteStartTimes[noteId];
		// Watchdog: a note held longer than any real note means its note-off was
		// lost. Force-close it instead of letting the rectangle grow forever.
		if(heldFor > STUCK_NOTE_TIMEOUT){
			_activeRecording[nid] = false;
			_actives[nid] = -1;
			continue;
		}
		note.duration = (std::max)(float(heldFor), 0.0f);
		_actives[nid] = int(note.set);
		// Keep track of which region was modified.
		minUpdated = (std::min)(minUpdated, noteId);
		maxUpdated = (std::max)(maxUpdated, noteId);
	}

	// Restore pedals to the last known state.
	_pedals = Pedals();
	auto nextBig = _pedalInfos.upper_bound(time);
	if(nextBig != _pedalInfos.begin()){
		_pedals = std::prev(nextBig)->second;
	}

	// Process new events.
	MIDIFrame frame;
	frame.timestamp = time;
	frame.messages.reserve(8);

	while(true){
		auto message = shared().get_message();
		if(message.size() == 0){
			// End of the queue.
			break;
		}

		// Store message for saving.
		frame.messages.push_back(message);

		const auto type = message.get_message_type();
		// Handle note events.
		if(message.is_note_on_or_off()){
			// Visual transpose (display only): shift the note used for all screen
			// bookkeeping (recording slot, highlight, geometry). The raw message stored
			// in _allMessages is untouched, so recordings export at original pitch.
			const short raw = clamp<short>(short(message[1]), 0, 127);
			const int shifted = int(raw) + _transpose;
			const bool onScreen = shifted >= 0 && shifted <= 127;
			const short note = short(shifted);
			const short velocity = clamp<short>(short(message[2]), 0, 127);

			if(_verbose){
				std::cout << "Note: " << int(note) << " " << int(velocity) << " " << (type == libremidi::message_type::NOTE_ON ? "on" : "off")<< "(" << message.timestamp << ")\n";
			}

			// End the note this key was drawing, on the slot it was drawn on at
			// note-on (even if the transpose changed since). Duration was updated above.
			releaseRawKey(raw);

			// If this is an on event with positive velocity, start a new note.
			// Keys transposed off the 0-127 range are recorded but not drawn.
			if(type == libremidi::message_type::NOTE_ON && velocity > 0 && onScreen){

				// Another key may still be drawing on this slot (transpose changed
				// while it was held): end it so the two don't share bookkeeping.
				if(_activeRecording[note]){
					_activeRecording[note] = false;
					_actives[note] = -1;
				}
				clearDisplayOwner(note);

				const int index = _notesCount % MAX_NOTES_IN_FLIGHT;
				// The ring buffer wraps after MAX_NOTES_IN_FLIGHT notes. If the slot we
				// are about to reuse still belongs to a note that is being recorded,
				// force-close that note first so its bookkeeping doesn't desync (which
				// would leave a rectangle growing forever).
				for(size_t pid = 0; pid < _activeRecording.size(); ++pid){
					if(_activeRecording[pid] && _activeIds[pid] == index){
						_activeRecording[pid] = false;
						_actives[pid] = -1;
						_activeIds[pid] = -1;
						clearDisplayOwner(short(pid));
					}
				}
				// Get new note.
				auto & newNote = _notes[index];
				newNote.start = float(time);
				_noteStartTimes[index] = time;
				newNote.duration = 0.0f;
				newNote.note = note;
				// Save the original channel.
				_notesInfos[index].channel = message.get_channel();
				// Compute set according to current setting.
				const int set = _currentSetOption.apply(int(newNote.note), _notesInfos[index].channel, 0, newNote.start);
				newNote.set = float(set);
				_actives[note] = int(newNote.set);
				// Activate recording of the key.
				_activeRecording[note] = true;
				_activeIds[note] = index;
				_rawToDisplay[raw] = note;

				// Compute proper rendering note.
				const bool isMin = noteIsMinor[note % 12];
				const short shiftId = (note/12) * 7 + noteShift[note % 12];
				newNote.isMinor = isMin ? 1.0f : 0.0f;
				newNote.note = float(shiftId);
				// Save original note.
				_notesInfos[index].note = note;

				// Keep track of which region was modified.
				minUpdated = (std::min)(minUpdated, int(index));
				maxUpdated = (std::max)(maxUpdated, int(index));

				// Find an available particles system and update it with the note parameters.
				for(auto & particle : _particles){
					if(particle.note < 0){
						// Update with new note parameter.
						//const float durationTweak = 3.0f - float(velocity) / 127.0f * 2.5f;
						particle.duration = _particlesDuration;
						particle.start = newNote.start;
						particle.note = note;
						particle.set = int(newNote.set);
						particle.elapsed = 0.0f;
						break;
					}
				}

				++_notesCount;
			}

		} else if(message.is_meta_event()){
			// Handle tempo and signature changes.
			const libremidi::meta_event_type metaType = message.get_meta_event_type();

			if(metaType == libremidi::meta_event_type::TIME_SIGNATURE){
				_signatureNum = double(message[3]);
				_signatureDenom = double(std::pow(2, short(message[4])));
				_secondsPerMeasure = computeMeasureDuration(_tempo, _signatureNum / _signatureDenom);

				if(_verbose){
					std::cout << "Signature: " << _signatureNum << "/" << _signatureDenom << " " <<  _secondsPerMeasure << "(" << message.timestamp << ")\n";
				}

			} else if(metaType == libremidi::meta_event_type::TEMPO_CHANGE){
				_tempo = int(((message[3] & 0xFF) << 16) | ((message[4] & 0xFF) << 8) | (message[5] & 0xFF));
				_secondsPerMeasure = computeMeasureDuration(_tempo, _signatureNum / _signatureDenom);
				if(_verbose){
					std::cout << "Tempo: " << _tempo << " " <<  _secondsPerMeasure << "(" << message.timestamp << ")\n";
				}
			} else {
				if(_verbose){
					std::cout << "Meta: " << "other (" << message.timestamp << ")\n";
				}
			}

		} else if(type == libremidi::message_type::CONTROL_CHANGE){

			// Handle pedal.
			const int rawType = clamp<int>(message[1], 0, 127);

			if(_verbose){
				std::cout << "Control: " << rawType << "(" << message.timestamp << ")\n";
			}

			// Skip other CC.
			if(rawType != 64 && rawType != 66 && rawType != 67 && rawType != 11){
				continue;
			}
			const PedalType type = PedalType(rawType);

			float & pedal = (type == DAMPER ? _pedals.damper : (type == SOSTENUTO ? _pedals.sostenuto : (type == SOFT ? _pedals.soft : _pedals.expression)));
			// Stop the current pedal.
			pedal = 0.0f;
			// If non-zero velocity, enable pedal, normalized velocity.
			const short val = clamp<short>(message[2], 0, 127);
			if(val > 0){
				pedal = float(val)/127.0f;
			}
			// Register new pedal event with updated state.
			_pedalInfos[time] = Pedals(_pedals);
			// Keep the pedal history bounded during very long sessions. We always
			// retain the most recent events (which hold the current pedal state).
			while(_pedalInfos.size() > MAX_STORED_PEDAL_EVENTS){
				_pedalInfos.erase(_pedalInfos.begin());
			}
		} else {
			if(_verbose){
				std::cout << "Other (" << message.timestamp << ")\n";
			}
		}

	}
	// Insert all messages treated this frame in a new frame.
	if(!frame.messages.empty()){
		_allMessages.push_back(std::move(frame));
		// Bound the saved-message history so a multi-hour session can't grow memory
		// without limit. Only the oldest frames (already off-screen) are dropped.
		// Trim in one large chunk so the costly front erase runs rarely, not every frame.
		if(_allMessages.size() > MAX_STORED_FRAMES){
			const size_t keep = MAX_STORED_FRAMES * 9 / 10;
			_allMessages.erase(_allMessages.begin(), _allMessages.begin() + (_allMessages.size() - keep));
			_historyTrimmed = true;
		}
	}

	// Update completed notes.
	for(size_t i = 0; i < _effectiveNotesCount; ++i){
		const auto & noteId = _notesInfos[i];
		// If the key is recording, no need to update _actives, skip.
		if(_activeRecording[noteId.note]){
			continue;
		}

		auto& note = _notes[i];
		// Use the double-precision start so onset/end detection stays exact even
		// when the timeline has been running for hours (a float start would only
		// resolve to ~8ms and could miss the one-frame windows below).
		const double noteStart = _noteStartTimes[i];
		const double noteEnd = noteStart + double(note.duration);
		// Ignore notes that ended at this frame.
		if(noteEnd > _previousTime && noteEnd <= time){
			continue;
		}
		// Update for notes currently playins.
		if(noteStart <= time && noteEnd >= time){
			_actives[noteId.note] = int(note.set);
		}
		// Detect notes that started at this frame.
		if(noteStart > _previousTime && noteStart <= time){
			// Find an available particles system and update it with the note parameters.
			for(auto & particle : _particles){
				if(particle.note < 0){
					// Update with new note parameter.
					particle.duration = _particlesDuration;
					particle.start = note.start;
					particle.note = noteId.note;
					particle.set = int(note.set);
					particle.elapsed = 0.0f;
					break;
				}
			}
		}
	}

	// If we have indeed updated a note, trigger an upload.
	if(minUpdated <= maxUpdated){
		_dirtyNotes = true;
		_dirtyNotesRange = {minUpdated, maxUpdated};
	}
	// Update range of notes to show.
	_effectiveNotesCount = std::min(MAX_NOTES_IN_FLIGHT, _notesCount);
	// Update timings.
	_previousTime = time;
	_maxTime = (std::max)(time, _maxTime);
}

double MIDISceneLive::duration() const {
	return _maxTime;
}

double MIDISceneLive::secondsPerMeasure() const {
	return _secondsPerMeasure;
}

int MIDISceneLive::notesCount() const {
	return _notesCount;
}

int MIDISceneLive::tracksCount() const {
	return 1;
}

void MIDISceneLive::print() const {
	std::cout << "[INFO]: Live scene with " << notesCount() << " notes, duration " << duration() << "s." << std::endl;
}

void MIDISceneLive::save(std::ofstream& file) const {

	const double quarterNotesPerSecond = 1000000.0 / double(_tempo);
	const double unitsPerQuarterNote = 960.0;
	const double unitsPerSecond = unitsPerQuarterNote * quarterNotesPerSecond;

	if(_verbose){
		std::cout << "Saving recording using " << unitsPerSecond << " units per second, containing " << _allMessages.size() << " messages." << std::endl;
	}

	// Make a copy of all messages and sort it.
	std::vector<MIDIFrame> allMessages(_allMessages);
	// Start by sorting the frames
	std::sort(allMessages.begin(), allMessages.end(), [](const MIDIFrame& a, const MIDIFrame& b){
		return a.timestamp < b.timestamp;
	});

	// For each frame, update all messages timestamp. If the oldest history was
	// dropped, start at the first kept frame instead of a long leading silence.
	double currentTime = (_historyTrimmed && !allMessages.empty()) ? allMessages.front().timestamp : 0.0;

	for(MIDIFrame& frame : allMessages){
		// Skip empty frames (should not exist), don't udpate the timing.
		if(frame.messages.empty()){
			continue;
		}
		// First message should have a real delta to the last existing message.
		frame.messages[0].timestamp = frame.timestamp - currentTime;
		// All others are 0 as they happen at the same time.
		const size_t messageCount = frame.messages.size();
		for(size_t mid = 1; mid < messageCount; ++mid){
			frame.messages[mid].timestamp = 0.0;
		}
		// If two consecutive frames have the same timestamp, all deltas of the second frame will be set to 0.
		currentTime = frame.timestamp;
	}

	// Start a file with one track.
	libremidi::writer writer;
	writer.ticksPerQuarterNote = int(unitsPerQuarterNote);
	writer.tracks.resize(1);

	// Set an initial tempo/signature at t=0 so that the first 'real' message delta is correct.
	writer.add_event(0, 0, libremidi::meta_events::tempo(_tempo));
	writer.add_event(0, 0, libremidi::meta_events::time_signature(int(_signatureNum), int(_signatureDenom)));
	writer.add_event(0, 0, libremidi::meta_events::key_signature(1, false));

	// Write all messages.
	for(const MIDIFrame& frame : allMessages){
		for(const libremidi::message& message : frame.messages){
			writer.add_event(int(message.timestamp * unitsPerSecond), 0, message);
		}
	}
	writer.write(file);
}

const std::string& MIDISceneLive::deviceName() const {
	return _deviceName;
}

void MIDISceneLive::resetStuckNotes(){
	// Force-close every recording note. The note keeps whatever duration it has
	// so far; it just stops growing. This recovers from a lost note-off without
	// needing to restart the app and reconnect the device.
	for(size_t nid = 0; nid < _activeRecording.size(); ++nid){
		_activeRecording[nid] = false;
		_actives[nid] = -1;
		_activeIds[nid] = -1;
	}
	_rawToDisplay.fill(-1);
	_dirtyNotes = true;
	_dirtyNotesRange = {0, 0};
}

void MIDISceneLive::releaseRawKey(short raw){
	const short display = _rawToDisplay[raw];
	if(display < 0){
		return;
	}
	if(_activeRecording[display]){
		_activeRecording[display] = false;
		_actives[display] = -1;
	}
	_rawToDisplay[raw] = -1;
}

void MIDISceneLive::clearDisplayOwner(short display){
	for(auto & slot : _rawToDisplay){
		if(slot == display){
			slot = -1;
		}
	}
}

libremidi::midi_in * MIDISceneLive::_sharedMIDIIn = nullptr;
std::vector<std::string> MIDISceneLive::_availablePorts;
int MIDISceneLive::_refreshIndex = 0;

libremidi::midi_in & MIDISceneLive::shared(){
	if(_sharedMIDIIn == nullptr){
		// The default queue (100 messages) drops events, note-offs included, when a slow
		// frame coincides with a burst of notes and pedal CCs. Use a much larger one.
		_sharedMIDIIn = new libremidi::midi_in(libremidi::API::UNSPECIFIED, "MIDIVisualizer", 4096);
	}
	return *_sharedMIDIIn;
}

const std::vector<std::string> & MIDISceneLive::availablePorts(bool force){
	if((_refreshIndex == 0) || force){
		const int portCount = shared().get_port_count();
		_availablePorts.resize(portCount);

		for(int i = 0; i < portCount; ++i){
			_availablePorts[i] = shared().get_port_name(i);
		}
	}
	// Only update once every 15 frames.
	_refreshIndex = (_refreshIndex + 1) % 15;
	return _availablePorts;
}
