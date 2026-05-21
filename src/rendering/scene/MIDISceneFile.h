#ifndef MIDISceneFile_h
#define MIDISceneFile_h
#include <gl3w/gl3w.h>
#include <glm/glm.hpp>
#include "../midi/MIDIFile.h"
#include "../State.h"
#include "MIDIScene.h"

#include <libremidi/libremidi.hpp>
#include <array>
#include <memory>

class MIDISceneFile : public MIDIScene {

public:

	MIDISceneFile(const std::string & midiFilePath, const SetOptions & options, const FilterOptions& filter );

	~MIDISceneFile();

	virtual void updateSetsAndVisibleNotes( const SetOptions& options, const FilterOptions& filter ) override;

	virtual void updateVisibleNotes( const FilterOptions& filter ) override;

	void updatesActiveNotes(double time, double speed, const FilterOptions& filter ) override;

	double duration() const override;

	double secondsPerMeasure() const override;

	int notesCount() const override;

	int tracksCount() const override;

	void print() const override;

	void save(std::ofstream& file) const override;

	const std::string& filePath() const;

	// MIDI output (drives an external sound engine like Pianoteq via a virtual port).
	static const std::vector<std::string> & availableOutputPorts(bool force = false);
	bool setMidiOutputByName(const std::string & portName); // empty string disables
	const std::string& midiOutputPortName() const { return _midiOutPortName; }
	void silenceAllNotes(); // sends note-off + all-notes-off, call on pause/restart

private:

	MIDIFile _midiFile;
	std::string _filePath;
	double _previousTime = 0.0;

	std::unique_ptr<libremidi::midi_out> _midiOut;
	std::array<bool, 128> _wasActive{};
	std::array<int, 4> _lastPedalCC{{-1, -1, -1, -1}}; // damper, sostenuto, soft, expression
	std::string _midiOutPortName; // empty when disabled

	static std::vector<std::string> _availableOutputPorts;
};

#endif
