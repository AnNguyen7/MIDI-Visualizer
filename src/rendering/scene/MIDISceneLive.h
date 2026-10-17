#ifndef MIDISceneLive_h
#define MIDISceneLive_h
#include <gl3w/gl3w.h>
#include <glm/glm.hpp>
#include "../midi/MIDIBase.h"
#include "../State.h"
#include "MIDIScene.h"

#include <libremidi/libremidi.hpp>
#include <map>

#define VIRTUAL_DEVICE_NAME "VIRTUAL"

class MIDISceneLive : public MIDIScene {

public:

	MIDISceneLive(int port, bool verbose);

	~MIDISceneLive();

	virtual void updateSetsAndVisibleNotes( const SetOptions& options, const FilterOptions& filter ) override;

	virtual void updateVisibleNotes( const FilterOptions& filter ) override;

	void updatesActiveNotes(double time, double speed, const FilterOptions& filter ) override;

	double duration() const override;

	double secondsPerMeasure() const override;

	int notesCount() const override;

	int tracksCount() const override;

	void print() const override;

	void save(std::ofstream& file) const override;

	const std::string& deviceName() const;

	// Force-close every note that is still being recorded. Used to recover from
	// stuck notes (a note-on whose note-off never arrived) without restarting.
	void resetStuckNotes();

	static const std::vector<std::string> & availablePorts(bool force = false);
	
private:

	/// End the note currently drawn for a physical key (note-off or retrigger).
	void releaseRawKey(short raw);

	/// Forget which physical key owns a display slot (the slot is being reused).
	void clearDisplayOwner(short display);

	struct NoteInfos {
		short note;
		short channel;
	};

	struct MIDIFrame {
		std::vector<libremidi::message> messages;
		double timestamp;
	};

	std::vector<NoteInfos> _notesInfos;
	// Note start times in double precision, indexed by ring-buffer slot. The GPU
	// note only stores a float start (fine for rendering); the lifecycle logic
	// uses these so onset/end detection stays exact even after long uptime.
	std::vector<double> _noteStartTimes;
	std::array<int, 128> _activeIds;
	std::array<bool, 128> _activeRecording;
	// Display slot (transposed pitch) each physical key was drawn on at note-on,
	// indexed by raw pitch, -1 if none. Note-offs use it so that changing the
	// transpose while keys are held still ends the right notes.
	std::array<short, 128> _rawToDisplay;
	std::map<double, Pedals> _pedalInfos;
	std::vector<MIDIFrame> _allMessages;
	bool _historyTrimmed = false; ///< Oldest frames of _allMessages were dropped.

	double _previousTime = 0.0;
	double _maxTime = 0.0;
	double _signatureNum = 4.0;
	double _signatureDenom = 4.0;
	double _secondsPerMeasure = 1.0;

	int _notesCount = 0;
	int _tempo = 500000;
	SetOptions _currentSetOption;
	std::string _deviceName;
	bool _verbose = false;

	static libremidi::midi_in & shared();

	static libremidi::midi_in * _sharedMIDIIn;
	static std::vector<std::string> _availablePorts;
	static int _refreshIndex;

};

#endif
