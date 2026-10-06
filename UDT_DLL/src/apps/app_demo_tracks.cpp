#include "parser.hpp"
#include "parser_plug_in.hpp"
#include "shared.hpp"
#include "utils.hpp"
#include "path.hpp"
#include "file_stream.hpp"
#include "file_system.hpp"
#include "look_up_tables.hpp"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>


#define    UDT_TRACKS_DEFAULT_RATE_MS    250
#define    UDT_TRACKS_PM_NORMAL          0
#define    UDT_TRACKS_PM_SPECTATOR       2


void PrintHelp()
{
	printf("Exports player tracks, kills and bullet impacts of an ETTV demo to JSON.\n");
	printf("\n");
	printf("UDT_tracks [-o=outputfolder] [-q] [-r=ms] inputfile\n");
	printf("\n");
	printf("-q    quiet mode: no logging to stdout        (default: off)\n");
	printf("-o=p  set the output folder path to p         (default: the input's folder)\n");
	printf("-r=N  sample player states every N ms         (default: %d)\n", UDT_TRACKS_DEFAULT_RATE_MS);
	printf("\n");
	printf("Writes <demo name>.tracks.json. Times are in ms since the demo's first snapshot.\n");
}

struct TrackSample
{
	s32 Time;
	s32 Slot;
	s32 Origin[3];
	s32 Yaw;
	s32 Health;
	s32 Alive;
	s32 Weapon;
	s32 Team;
};

struct TrackKill
{
	s32 Time;
	s32 Attacker; // -1 for the world
	s32 Victim;
	s32 AttackerOrigin[3];
	s32 VictimOrigin[3];
	u32 MeanOfDeath;
	bool HasAttackerOrigin;
};

struct TrackShot
{
	s32 Time;
	s32 Shooter;
	s32 Target; // -1 when no player was hit
	s32 Origin[3];
	s32 Weapon;
	s32 Hit; // 0 none, 1 team, 2 head, 3 body
};

struct TrackPhase
{
	s32 Time;
	s32 Phase;
};

struct TrackName
{
	udtString Name;
	s32 Slot;
};

static void CopyOrigin(s32* output, const idVec3& input)
{
	for(u32 i = 0; i < 3; ++i)
	{
		output[i] = (s32)input[i];
	}
}

struct udtParserPlugInTracks : udtBaseParserPlugIn
{
	udtParserPlugInTracks(s32 rateMs)
	{
		_rateMs = rateMs;
		_startTime = UDT_S32_MIN;
		_nextSampleTime = UDT_S32_MIN;
		_lastPhase = UDT_S32_MIN;
		_newSnapshot = false;
		_map = udtString::NewEmptyConstant();
	}

	void InitAllocators(u32 /*demoCount*/) override
	{
	}

	void ProcessGamestateMessage(const udtGamestateCallbackArg& /*arg*/, udtBaseParser& parser) override
	{
		const s32 serverInfo = GetIdNumber(udtMagicNumberType::ConfigStringIndex, udtConfigStringIndex::ServerInfo, parser._inProtocol);
		udtString map;
		if(_map.GetLength() == 0 &&
		   ParseConfigStringValueString(map, *TempAllocator, "mapname", parser.GetConfigString(serverInfo).GetPtr()))
		{
			_map = udtString::NewClone(_stringAllocator, map.GetPtr());
		}

		const s32 firstPlayer = GetIdNumber(udtMagicNumberType::ConfigStringIndex, udtConfigStringIndex::FirstPlayer, parser._inProtocol);
		for(s32 i = 0; i < ID_MAX_CLIENTS; ++i)
		{
			AddName(parser, i, firstPlayer + i);
		}
		UpdatePhase(parser);
	}

	void ProcessCommandMessage(const udtCommandCallbackArg& arg, udtBaseParser& parser) override
	{
		if(!arg.IsConfigString)
		{
			return;
		}

		const s32 firstPlayer = GetIdNumber(udtMagicNumberType::ConfigStringIndex, udtConfigStringIndex::FirstPlayer, parser._inProtocol);
		const s32 wolfInfo = GetIdNumber(udtMagicNumberType::ConfigStringIndex, udtConfigStringIndex::Wolf_Info, parser._inProtocol);
		if(arg.ConfigStringIndex == wolfInfo)
		{
			UpdatePhase(parser);
		}
		else if(arg.ConfigStringIndex >= firstPlayer && arg.ConfigStringIndex < firstPlayer + ID_MAX_CLIENTS)
		{
			AddName(parser, arg.ConfigStringIndex - firstPlayer, arg.ConfigStringIndex);
		}
	}

	void ProcessSnapshotMessage(const udtSnapshotCallbackArg& arg, udtBaseParser& parser) override
	{
		if(_startTime == UDT_S32_MIN)
		{
			_startTime = arg.ServerTime;
		}
		_newSnapshot = true;

		const udtProtocol::Id protocol = parser._inProtocol;
		const s32 eventType = GetIdNumber(udtMagicNumberType::EntityType, udtEntityType::Event, protocol, parser._inMod);
		const s32 bulletEvent = GetIdNumber(udtMagicNumberType::EntityEvent, udtEntityEvent::Wolf_Bullet, protocol, parser._inMod);
		for(u32 i = 0; i < arg.ChangedEntityCount; ++i)
		{
			if(!arg.ChangedEntities[i].IsNewEvent)
			{
				continue;
			}

			const idEntityStateBase& entity = *arg.ChangedEntities[i].Entity;
			udtObituaryEvent obituary;
			if(IsObituaryEvent(obituary, entity, protocol, parser._inMod))
			{
				TrackKill kill;
				memset(&kill, 0, sizeof(kill));
				kill.Time = RelativeTime(arg.ServerTime);
				kill.Attacker = obituary.AttackerIndex;
				kill.Victim = obituary.TargetIndex;
				kill.MeanOfDeath = obituary.MeanOfDeath;
				CopyOrigin(kill.VictimOrigin, entity.pos.trBase);
				_pendingKills.Add(kill);
				continue;
			}

			if(bulletEvent != UDT_S32_MIN && (entity.eType & ~ID_ES_EVENT_BITS) == eventType + bulletEvent)
			{
				TrackShot shot;
				shot.Time = RelativeTime(arg.ServerTime);
				shot.Shooter = entity.otherEntityNum;
				shot.Target = entity.otherEntityNum2 >= 0 && entity.otherEntityNum2 < ID_MAX_CLIENTS ? entity.otherEntityNum2 : -1;
				CopyOrigin(shot.Origin, entity.pos.trBase);
				shot.Weapon = entity.weapon;
				shot.Hit = entity.modelindex;
				_shots.Add(shot);
			}
		}
	}

	// The ETTV player states of a snapshot follow it in the same message.
	void ProcessMessageBundleEnd(const udtMessageBundleCallbackArg& /*arg*/, udtBaseParser& parser) override
	{
		if(!_newSnapshot || parser._inProtocol != udtProtocol::Dm284)
		{
			return;
		}
		_newSnapshot = false;

		idClientSnapshotBase* const snapshot = parser.GetClientSnapshot(parser._inLastSnapshotMessageNumber & PACKET_MASK);
		for(u32 i = 0, count = _pendingKills.GetSize(); i < count; ++i)
		{
			TrackKill kill = _pendingKills[i];
			const idPlayerState84* victim = GetPlayer(snapshot, kill.Victim);
			const idPlayerState84* attacker = GetPlayer(snapshot, kill.Attacker);
			if(victim != NULL)
			{
				CopyOrigin(kill.VictimOrigin, victim->origin);
			}
			if(attacker != NULL)
			{
				CopyOrigin(kill.AttackerOrigin, attacker->origin);
				kill.HasAttackerOrigin = true;
			}
			_kills.Add(kill);
		}
		_pendingKills.Clear();

		const s32 time = RelativeTime(snapshot->serverTime);
		if(_nextSampleTime != UDT_S32_MIN && time < _nextSampleTime)
		{
			return;
		}
		_nextSampleTime = time + _rateMs;

		for(s32 slot = 0; slot < ID_MAX_CLIENTS; ++slot)
		{
			const idPlayerState84* ps = GetPlayer(snapshot, slot);
			if(ps == NULL || ps->pm_type == UDT_TRACKS_PM_SPECTATOR)
			{
				continue;
			}

			TrackSample sample;
			sample.Time = time;
			sample.Slot = slot;
			CopyOrigin(sample.Origin, ps->origin);
			sample.Yaw = (((s32)ps->viewangles[1] + 180) % 360 + 360) % 360 - 180;
			sample.Health = (s32)(s16)ps->stats[0];
			sample.Alive = ps->pm_type == UDT_TRACKS_PM_NORMAL && sample.Health > 0 ? 1 : 0;
			sample.Weapon = ps->weapon;
			sample.Team = ps->teamNum;
			_samples.Add(sample);
		}
	}

	bool Write(const char* filePath) const
	{
		FILE* const file = fopen(filePath, "wb");
		if(file == NULL)
		{
			return false;
		}

		fprintf(file, "{\"v\":1,\"map\":");
		WriteString(file, _map.GetPtr());
		fprintf(file, ",\"rate_ms\":%d,\"phases\":[", _rateMs);
		for(u32 i = 0, count = _phases.GetSize(); i < count; ++i)
		{
			fprintf(file, "%s[%d,\"%s\"]", i ? "," : "", _phases[i].Time, GetPhaseName(_phases[i].Phase));
		}

		fprintf(file, "],\"slots\":{");
		bool firstSlot = true;
		for(s32 slot = 0; slot < ID_MAX_CLIENTS; ++slot)
		{
			bool firstName = true;
			for(u32 i = 0, count = _names.GetSize(); i < count; ++i)
			{
				if(_names[i].Slot != slot)
				{
					continue;
				}
				fprintf(file, "%s", firstName ? (firstSlot ? "" : ",") : ",");
				if(firstName)
				{
					fprintf(file, "\"%d\":[", slot);
				}
				WriteString(file, _names[i].Name.GetPtr());
				firstName = false;
				firstSlot = false;
			}
			if(!firstName)
			{
				fprintf(file, "]");
			}
		}

		fprintf(file, "},\"samples\":{");
		WriteColumn(file, "t", offsetof(TrackSample, Time), true);
		WriteColumn(file, "slot", offsetof(TrackSample, Slot), false);
		WriteColumn(file, "x", offsetof(TrackSample, Origin), false);
		WriteColumn(file, "y", offsetof(TrackSample, Origin) + sizeof(s32), false);
		WriteColumn(file, "z", offsetof(TrackSample, Origin) + 2 * sizeof(s32), false);
		WriteColumn(file, "yaw", offsetof(TrackSample, Yaw), false);
		WriteColumn(file, "hp", offsetof(TrackSample, Health), false);
		WriteColumn(file, "alive", offsetof(TrackSample, Alive), false);
		WriteColumn(file, "weapon", offsetof(TrackSample, Weapon), false);
		WriteColumn(file, "team", offsetof(TrackSample, Team), false);

		fprintf(file, "},\"kills\":[");
		for(u32 i = 0, count = _kills.GetSize(); i < count; ++i)
		{
			const TrackKill& k = _kills[i];
			fprintf(file, "%s[%d,%d,%d,", i ? "," : "", k.Time, k.Attacker, k.Victim);
			if(k.HasAttackerOrigin)
			{
				fprintf(file, "%d,%d,%d,", k.AttackerOrigin[0], k.AttackerOrigin[1], k.AttackerOrigin[2]);
			}
			else
			{
				fprintf(file, "null,null,null,");
			}
			fprintf(file, "%d,%d,%d,\"%s\"]", k.VictimOrigin[0], k.VictimOrigin[1], k.VictimOrigin[2], GetUDTModName((s32)k.MeanOfDeath));
		}

		fprintf(file, "],\"shots\":[");
		for(u32 i = 0, count = _shots.GetSize(); i < count; ++i)
		{
			const TrackShot& s = _shots[i];
			fprintf(file, "%s[%d,%d,%d,%d,%d,%d,%d,%d]", i ? "," : "",
					s.Time, s.Shooter, s.Target, s.Origin[0], s.Origin[1], s.Origin[2], s.Weapon, s.Hit);
		}
		fprintf(file, "]}\n");

		return fclose(file) == 0;
	}

	u32 GetSampleCount() const { return _samples.GetSize(); }
	u32 GetKillCount() const { return _kills.GetSize(); }
	u32 GetShotCount() const { return _shots.GetSize(); }

private:
	s32 RelativeTime(s32 serverTime) const
	{
		return (_startTime == UDT_S32_MIN || serverTime < _startTime) ? 0 : serverTime - _startTime;
	}

	const idPlayerState84* GetPlayer(idClientSnapshotBase* snapshot, s32 slot) const
	{
		if(slot < 0 || slot >= ID_MAX_CLIENTS || !GetTvSnapshot(snapshot, udtProtocol::Dm284, slot)->valid)
		{
			return NULL;
		}

		return (const idPlayerState84*)GetTvPlayerState(snapshot, udtProtocol::Dm284, slot);
	}

	void UpdatePhase(udtBaseParser& parser)
	{
		const s32 wolfInfo = GetIdNumber(udtMagicNumberType::ConfigStringIndex, udtConfigStringIndex::Wolf_Info, parser._inProtocol);
		s32 phase = 0;
		if(!ParseConfigStringValueInt(phase, *TempAllocator, "gamestate", parser.GetConfigString(wolfInfo).GetPtr()) ||
		   phase == _lastPhase)
		{
			return;
		}

		TrackPhase entry;
		entry.Time = RelativeTime(parser._inServerTime);
		entry.Phase = phase;
		_phases.Add(entry);
		_lastPhase = phase;
	}

	void AddName(udtBaseParser& parser, s32 slot, s32 csIndex)
	{
		udtString name;
		if(!ParseConfigStringValueString(name, *TempAllocator, "n", parser.GetConfigString(csIndex).GetPtr()) ||
		   name.GetLength() == 0)
		{
			return;
		}

		for(u32 i = 0, count = _names.GetSize(); i < count; ++i)
		{
			if(_names[i].Slot == slot && udtString::Equals(_names[i].Name, name))
			{
				return;
			}
		}

		TrackName entry;
		entry.Name = udtString::NewClone(_stringAllocator, name.GetPtr());
		entry.Slot = slot;
		_names.Add(entry);
	}

	void WriteColumn(FILE* file, const char* name, size_t offset, bool first) const
	{
		fprintf(file, "%s\"%s\":[", first ? "" : ",", name);
		for(u32 i = 0, count = _samples.GetSize(); i < count; ++i)
		{
			fprintf(file, "%s%d", i ? "," : "", *(const s32*)((const u8*)&_samples[i] + offset));
		}
		fprintf(file, "]");
	}

	static const char* GetPhaseName(s32 phase)
	{
		switch(phase)
		{
			case 0: return "playing";
			case 1: return "countdown";
			case 2: return "warmup";
			case 3: return "intermission";
			default: return "other";
		}
	}

	// Names are raw game bytes, not UTF-8: each byte becomes its Latin-1 code point.
	static void WriteString(FILE* file, const char* string)
	{
		fputc('"', file);
		for(const u8* c = (const u8*)string; *c != '\0'; ++c)
		{
			if(*c == '"' || *c == '\\')
			{
				fprintf(file, "\\%c", *c);
			}
			else if(*c < 0x20 || *c >= 0x7F)
			{
				fprintf(file, "\\u%04x", *c);
			}
			else
			{
				fputc(*c, file);
			}
		}
		fputc('"', file);
	}

	udtVMLinearAllocator _stringAllocator { "Tracks::Strings" };
	udtVMArray<TrackSample> _samples { "Tracks::Samples" };
	udtVMArray<TrackKill> _kills { "Tracks::Kills" };
	udtVMArray<TrackKill> _pendingKills { "Tracks::PendingKills" };
	udtVMArray<TrackShot> _shots { "Tracks::Shots" };
	udtVMArray<TrackPhase> _phases { "Tracks::Phases" };
	udtVMArray<TrackName> _names { "Tracks::Names" };
	udtString _map;
	s32 _rateMs;
	s32 _startTime;
	s32 _nextSampleTime;
	s32 _lastPhase;
	bool _newSnapshot;
};

static bool ExportTracks(const char* inputPath, const char* outputFolder, s32 rateMs)
{
	udtVMLinearAllocator allocator("ExportTracks::Temp");
	const udtString input = udtString::NewConstRef(inputPath);
	udtString folder, name, outputPath;
	if(outputFolder != NULL)
	{
		folder = udtString::NewConstRef(outputFolder);
	}
	else if(!udtPath::GetFolderPath(folder, allocator, input))
	{
		folder = udtString::NewConstRef(".");
	}
	udtPath::GetFileNameWithoutExtension(name, allocator, input);
	const udtString fileName = udtString::NewFromConcatenating(allocator, name, udtString::NewConstRef(".tracks.json"));
	udtPath::Combine(outputPath, allocator, folder, fileName);

	udtContext context;
	context.SetCallbacks(&CallbackConsoleMessage, NULL, NULL, NULL);

	udtVMLinearAllocator plugInAllocator("ExportTracks::PlugIn");
	udtParserPlugInTracks plugIn(rateMs);
	plugIn.Init(1, plugInAllocator);

	udtBaseParser parser;
	parser.AddPlugIn(&plugIn);

	udtFileStream file;
	if(!file.Open(inputPath, udtFileOpenMode::Read))
	{
		fprintf(stderr, "Failed to open %s\n", inputPath);
		return false;
	}

	if(!parser.Init(&context, udtProtocol::Dm284, udtProtocol::Dm284))
	{
		return false;
	}
	parser.SetFilePath(inputPath);
	if(!RunParser(parser, file, NULL))
	{
		fprintf(stderr, "Failed to parse %s\n", inputPath);
		return false;
	}

	if(!plugIn.Write(outputPath.GetPtr()))
	{
		fprintf(stderr, "Failed to write %s\n", outputPath.GetPtr());
		return false;
	}

	char message[512];
	sprintf(message, "Wrote %s: %u samples, %u kills, %u shots", outputPath.GetPtr(), plugIn.GetSampleCount(), plugIn.GetKillCount(), plugIn.GetShotCount());
	CallbackConsoleMessage(0, message);

	return true;
}

int udt_main(int argc, char** argv)
{
	if(argc < 2)
	{
		PrintHelp();
		return 0;
	}

	const char* const inputPath = argv[argc - 1];
	if(!udtFileStream::Exists(inputPath) ||
	   (udtProtocol::Id)udtGetProtocolByFilePath(inputPath) != udtProtocol::Dm284)
	{
		fprintf(stderr, "The input must be an existing .tv_84 demo.\n");
		return 1;
	}

	const char* outputFolder = NULL;
	s32 rateMs = UDT_TRACKS_DEFAULT_RATE_MS;
	for(int i = 1; i < argc - 1; ++i)
	{
		const udtString arg = udtString::NewConstRef(argv[i]);
		s32 localRate = 0;
		if(udtString::StartsWith(arg, "-o=") &&
		   arg.GetLength() >= 4 &&
		   IsValidDirectory(argv[i] + 3))
		{
			outputFolder = argv[i] + 3;
		}
		else if(udtString::StartsWith(arg, "-r=") &&
				arg.GetLength() >= 4 &&
				StringParseInt(localRate, arg.GetPtr() + 3) &&
				localRate >= 50)
		{
			rateMs = localRate;
		}
	}

	return ExportTracks(inputPath, outputFolder, rateMs) ? 0 : 1;
}
