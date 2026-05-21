#include <stdio.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <glm/gtc/matrix_transform.hpp>
#include <iterator>

#include "../../helpers/ProgramUtilities.h"
#include "../../helpers/ResourcesManager.h"

#include "MIDISceneFile.h"

#ifdef _WIN32
#undef MIN
#undef MAX
#endif

std::vector<std::string> MIDISceneFile::_availableOutputPorts;

MIDISceneFile::~MIDISceneFile(){
	silenceAllNotes();
	if(_midiOut){
		if(_midiOut->is_port_open()){
			_midiOut->close_port();
		}
		_midiOut.reset();
	}
}

const std::vector<std::string> & MIDISceneFile::availableOutputPorts(bool force){
	if(force || _availableOutputPorts.empty()){
		try {
			libremidi::midi_out probe(libremidi::API::UNSPECIFIED, "MIDIVisualizer probe");
			const unsigned int n = probe.get_port_count();
			_availableOutputPorts.clear();
			_availableOutputPorts.reserve(n);
			for(unsigned int i = 0; i < n; ++i){
				_availableOutputPorts.push_back(probe.get_port_name(i));
			}
		} catch(const std::exception & e){
			std::cerr << "[MIDI]: Unable to enumerate output ports: " << e.what() << std::endl;
			_availableOutputPorts.clear();
		}
	}
	return _availableOutputPorts;
}

bool MIDISceneFile::setMidiOutputByName(const std::string & portName){
	// Silence anything currently playing through the old port before swapping.
	silenceAllNotes();
	if(_midiOut && _midiOut->is_port_open()){
		_midiOut->close_port();
	}
	_midiOutPortName.clear();

	if(portName.empty()){
		return true; // disabled, success
	}

	const auto & ports = availableOutputPorts(true);
	int matchIndex = -1;
	for(size_t i = 0; i < ports.size(); ++i){
		if(ports[i] == portName){
			matchIndex = int(i);
			break;
		}
	}
	if(matchIndex < 0){
		std::cerr << "[MIDI]: Output port \"" << portName << "\" not found." << std::endl;
		return false;
	}

	try {
		if(!_midiOut){
			_midiOut.reset(new libremidi::midi_out(libremidi::API::UNSPECIFIED, "MIDIVisualizer"));
		}
		_midiOut->open_port(matchIndex, "MIDIVisualizer output");
		_midiOutPortName = portName;
		_wasActive.fill(false);
		return true;
	} catch(const std::exception & e){
		std::cerr << "[MIDI]: Failed to open output port \"" << portName << "\": " << e.what() << std::endl;
		_midiOut.reset();
		return false;
	}
}

void MIDISceneFile::silenceAllNotes(){
	if(!_midiOut || !_midiOut->is_port_open()){
		_wasActive.fill(false);
		_lastPedalCC = {-1, -1, -1, -1};
		return;
	}
	// CC 123 (All Notes Off) on every channel, plus explicit note-offs we know are active.
	for(int i = 0; i < 128; ++i){
		if(_wasActive[i]){
			unsigned char off[3] = {0x80, (unsigned char)i, 0};
			_midiOut->send_message(off, 3);
		}
	}
	for(int ch = 0; ch < 16; ++ch){
		unsigned char allOff[3] = {(unsigned char)(0xB0 | ch), 123, 0};
		_midiOut->send_message(allOff, 3);
	}
	// Release all pedals on channel 0 (damper, sostenuto, soft, expression).
	const unsigned char ccNums[4] = {64, 66, 67, 11};
	for(int p = 0; p < 4; ++p){
		unsigned char msg[3] = {0xB0, ccNums[p], 0};
		_midiOut->send_message(msg, 3);
	}
	_wasActive.fill(false);
	_lastPedalCC = {-1, -1, -1, -1};
}

MIDISceneFile::MIDISceneFile(const std::string & midiFilePath, const SetOptions & options, const FilterOptions& filter) : MIDIScene() {

	_filePath = midiFilePath;
	// MIDI processing.
	_midiFile = MIDIFile(_filePath);

	updateSetsAndVisibleNotes( options, filter );

	std::cout << "[INFO]: Final track duration " << _midiFile.duration() << " sec." << std::endl;
}


void MIDISceneFile::updateSetsAndVisibleNotes( const SetOptions& options, const FilterOptions& filter )
{
	_midiFile.updateSets( options );
	updateVisibleNotes( filter );
}

void MIDISceneFile::updateVisibleNotes( const FilterOptions& filter )
{
	// Generate note data for rendering.
	std::vector<MIDINote> notesM;
	_midiFile.getNotes( notesM, NoteType::MAJOR, filter, 0 );
	std::vector<MIDINote> notesm;
	_midiFile.getNotes( notesm, NoteType::MINOR, filter, 0 );

	// Load notes shared data.
	const size_t majorCount = notesM.size();
	const size_t minorCount = notesm.size();
	const size_t totalCount = majorCount + minorCount;
	_notes.resize( totalCount );

	for( size_t i = 0; i < majorCount; ++i )
	{
		const MIDINote& note = notesM[ i ];
		GPUNote& data = _notes[ i ];
		data.note = float( note.note );
		data.start = float( note.start );
		data.duration = float( note.duration );
		data.isMinor = 0.0f;
		data.set = float( note.set );
	}

	for( size_t i = 0; i < minorCount; ++i )
	{
		const MIDINote& note = notesm[ i ];
		GPUNote& data = _notes[ i + majorCount ];
		data.note = float( note.note );
		data.start = float( note.start );
		data.duration = float( note.duration );
		data.isMinor = 1.0f;
		data.set = float( note.set );
	}
	// Upload to the GPU.
	assert( totalCount < ( 1 << 31 ) );
	_dirtyNotes = true;
	_effectiveNotesCount = int( totalCount );
	_dirtyNotesRange = { 0, 0 }; // Means full array
}

void MIDISceneFile::updatesActiveNotes(double time, double speed, const FilterOptions& filter){
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
	// Get notes actives.
	auto actives = ActiveNotesArray();
	_midiFile.getNotesActive(actives, time, filter, 0);
	for(int i = 0; i < 128; ++i){
		const auto & note = actives[i];
		_actives[i] = note.enabled ? note.set : -1;
		// Check if the note was triggered at this frame.
		if(note.start > _previousTime && note.start <= time){
			// Find an available particles system and update it with the note parameters.
			for(auto & particle : _particles){
				if(particle.note < 0){
					// Update with new note parameter.
					//const float durationTweak = 3.0f - note.velocity / 127.0f * 2.5f;
					particle.duration = (std::max)(note.duration*2.0f, note.duration + 1.2f);
					particle.start = note.start;
					particle.note = i;
					particle.set = note.set;
					particle.elapsed = 0.0f;
					break;
				}
			}
		}
	}
	_previousTime = time;

	// Send MIDI output messages for note transitions (drives Pianoteq or any external sound engine).
	if(_midiOut && _midiOut->is_port_open()){
		try {
			for(int i = 0; i < 128; ++i){
				const bool nowActive = actives[i].enabled;
				if(nowActive && !_wasActive[i]){
					int vel = int(actives[i].velocity);
					if(vel < 1) vel = 80; // some files mark velocity 0 as note-off; safe default
					if(vel > 127) vel = 127;
					unsigned char on[3] = {0x90, (unsigned char)i, (unsigned char)vel};
					_midiOut->send_message(on, 3);
				} else if(!nowActive && _wasActive[i]){
					unsigned char off[3] = {0x80, (unsigned char)i, 0};
					_midiOut->send_message(off, 3);
				}
				_wasActive[i] = nowActive;
			}
		} catch(const std::exception & e){
			std::cerr << "[MIDI]: send_message failed: " << e.what() << " (disabling output)" << std::endl;
			_midiOut.reset();
			_midiOutPortName.clear();
			_wasActive.fill(false);
		}
	}

	// Update pedal state.
	_pedals.damper = _pedals.sostenuto = _pedals.soft = _pedals.expression = 0.0f;
	_midiFile.getPedalsActive(_pedals.damper, _pedals.sostenuto, _pedals.soft, _pedals.expression, time, 0);

	// Forward pedal changes as Control Change messages so VSTs respond to sustain/sostenuto/soft.
	if(_midiOut && _midiOut->is_port_open()){
		const float vals[4] = {_pedals.damper, _pedals.sostenuto, _pedals.soft, _pedals.expression};
		const unsigned char ccNums[4] = {64, 66, 67, 11};
		try {
			for(int p = 0; p < 4; ++p){
				int cc = int(glm::clamp(vals[p], 0.0f, 1.0f) * 127.0f + 0.5f);
				if(cc != _lastPedalCC[p]){
					unsigned char msg[3] = {0xB0, ccNums[p], (unsigned char)cc};
					_midiOut->send_message(msg, 3);
					_lastPedalCC[p] = cc;
				}
			}
		} catch(const std::exception & e){
			std::cerr << "[MIDI]: pedal CC failed: " << e.what() << std::endl;
		}
	}
}

double MIDISceneFile::duration() const {
	return _midiFile.duration();
}

double MIDISceneFile::secondsPerMeasure() const {
	return _midiFile.secondsPerMeasure();
}

int MIDISceneFile::notesCount() const {
	return _midiFile.notesCount();
}

int MIDISceneFile::tracksCount() const {
	return _midiFile.tracksCount();
}

void MIDISceneFile::print() const {
	_midiFile.print();
}

void MIDISceneFile::save(std::ofstream& file) const {
	// Do it the dumb way.
	std::ifstream input(_filePath);
	if(input.is_open() && file.is_open()){
		std::copy(std::istreambuf_iterator<char>(input),
				  std::istreambuf_iterator<char>(),
				  std::ostream_iterator<char>(file));
	}
	input.close();
}

const std::string& MIDISceneFile::filePath() const {
	return _filePath;
}
