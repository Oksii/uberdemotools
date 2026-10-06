#include "parser.hpp"
#include "parser_plug_in.hpp"
#include "shared.hpp"
#include "utils.hpp"
#include "path.hpp"
#include "file_stream.hpp"
#include "file_system.hpp"
#include "look_up_tables.hpp"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>


#define    UDT_TRACKS_DEFAULT_RATE_MS    250
#define    UDT_TRACKS_PM_NORMAL          0
#define    UDT_TRACKS_PM_SPECTATOR       2
// ET: Legacy trType_t values (bg_public.h), not Quake 3's.
#define    UDT_TRACKS_TR_STATIONARY      0
#define    UDT_TRACKS_TR_LINEAR          2
#define    UDT_TRACKS_TR_GRAVITY         6
#define    UDT_TRACKS_TR_GRAVITY_LOW     7
#define    UDT_TRACKS_TR_GRAVITY_FLOAT   8
#define    UDT_TRACKS_GRAVITY            800


void PrintHelp()
{
	printf("Exports player tracks, kills and missile flights of an ETTV demo to JSON.\n");
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

struct TrackMissile
{
	s32 SpawnTime;
	s32 Slot;
	s32 Weapon;
	s32 EndTime;
	s32 End[3];
};

// A point a missile's flight passed: its spawn, and every bounce or stop.
struct TrackMissilePoint
{
	u32 Missile;
	s32 Time;
	s32 Origin[3];
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
		_snapshotCount = 0;
		_map = udtString::NewEmptyConstant();
		for(s32 i = 0; i < MAX_GENTITIES; ++i)
		{
			_activeMissile[i] = -1;
		}
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
		TrackMissiles(arg, GetIdNumber(udtMagicNumberType::EntityType, udtEntityType::Missile, protocol, parser._inMod));
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

	// Missiles still in flight when the demo ends stop where they were last seen.
	void Finish()
	{
		for(s32 n = 0; n < MAX_GENTITIES; ++n)
		{
			if(_activeMissile[n] >= 0)
			{
				EndMissileWhereLastSeen(n);
			}
		}
	}

	bool Write(const char* filePath) const
	{
		FILE* const file = fopen(filePath, "wb");
		if(file == NULL)
		{
			return false;
		}

		fprintf(file, "{\"v\":2,\"map\":");
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

		fprintf(file, "],\"missiles\":[");
		for(u32 i = 0, count = _missiles.GetSize(); i < count; ++i)
		{
			const TrackMissile& m = _missiles[i];
			fprintf(file, "%s[%d,%d,%d,[", i ? "," : "", m.SpawnTime, m.Slot, m.Weapon);
			bool firstPoint = true;
			for(u32 j = 0, pointCount = _missilePoints.GetSize(); j < pointCount; ++j)
			{
				const TrackMissilePoint& p = _missilePoints[j];
				if(p.Missile != i)
				{
					continue;
				}
				fprintf(file, "%s[%d,%d,%d,%d]", firstPoint ? "" : ",", p.Time, p.Origin[0], p.Origin[1], p.Origin[2]);
				firstPoint = false;
			}
			fprintf(file, "],[%d,%d,%d,%d]]", m.EndTime, m.End[0], m.End[1], m.End[2]);
		}
		fprintf(file, "]}\n");

		return fclose(file) == 0;
	}

	u32 GetSampleCount() const { return _samples.GetSize(); }
	u32 GetKillCount() const { return _kills.GetSize(); }
	u32 GetMissileCount() const { return _missiles.GetSize(); }

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

	// A missile keeps its entity number from spawn to blast. Each new trajectory (the
	// throw, a bounce, coming to rest) is a point of its path; at the blast the entity
	// turns into a general one at the explosion's origin.
	void TrackMissiles(const udtSnapshotCallbackArg& arg, s32 missileType)
	{
		++_snapshotCount;
		const s32 time = RelativeTime(arg.ServerTime);
		for(u32 i = 0; i < arg.EntityCount; ++i)
		{
			const idEntityStateBase& es = *arg.Entities[i];
			const s32 n = es.number;
			if(n < 0 || n >= MAX_GENTITIES)
			{
				continue;
			}

			const s32 active = _activeMissile[n];
			const bool isMissile = es.eType == missileType && es.clientNum >= 0 && es.clientNum < ID_MAX_CLIENTS;
			if(!isMissile)
			{
				if(active >= 0)
				{
					s32 origin[3];
					CopyOrigin(origin, es.pos.trBase);
					EndMissile(n, time, origin);
				}
				continue;
			}

			if(active >= 0 && es.weapon != _missiles[active].Weapon)
			{
				EndMissileWhereLastSeen(n);
			}

			if(_activeMissile[n] < 0)
			{
				TrackMissile missile;
				missile.SpawnTime = RelativeTime(es.pos.trTime);
				missile.Slot = es.clientNum;
				missile.Weapon = es.weapon;
				missile.EndTime = 0;
				memset(missile.End, 0, sizeof(missile.End));
				_activeMissile[n] = (s32)_missiles.GetSize();
				_missiles.Add(missile);
				AddMissilePoint(n, es.pos, time);
			}
			else if(TrajectoryChanged(_lastTrajectory[n], es.pos))
			{
				AddMissilePoint(n, es.pos, time);
			}
			_lastTrajectory[n] = es.pos;
			_lastSeenTime[n] = arg.ServerTime;
			_lastSeenSnapshot[n] = _snapshotCount;
		}

		// Freed without turning into an explosion first.
		for(s32 n = 0; n < MAX_GENTITIES; ++n)
		{
			if(_activeMissile[n] >= 0 && _lastSeenSnapshot[n] != _snapshotCount)
			{
				EndMissileWhereLastSeen(n);
			}
		}
	}

	// A missile at rest has no trajectory time (G_SetOrigin zeroes it): it got there by this snapshot.
	void AddMissilePoint(s32 number, const idTrajectoryBase& trajectory, s32 snapshotTime)
	{
		TrackMissilePoint point;
		point.Missile = (u32)_activeMissile[number];
		point.Time = (s32)trajectory.trType == UDT_TRACKS_TR_STATIONARY ? snapshotTime : RelativeTime(trajectory.trTime);
		CopyOrigin(point.Origin, trajectory.trBase);
		_missilePoints.Add(point);
	}

	void EndMissile(s32 number, s32 time, const s32* origin)
	{
		TrackMissile& missile = _missiles[(u32)_activeMissile[number]];
		missile.EndTime = time;
		memcpy(missile.End, origin, sizeof(missile.End));
		_activeMissile[number] = -1;
	}

	void EndMissileWhereLastSeen(s32 number)
	{
		s32 origin[3];
		EvaluateTrajectory(origin, _lastTrajectory[number], _lastSeenTime[number]);
		EndMissile(number, RelativeTime(_lastSeenTime[number]), origin);
	}

	static bool TrajectoryChanged(const idTrajectoryBase& a, const idTrajectoryBase& b)
	{
		return a.trType != b.trType || a.trTime != b.trTime ||
			a.trBase[0] != b.trBase[0] || a.trBase[1] != b.trBase[1] || a.trBase[2] != b.trBase[2] ||
			a.trDelta[0] != b.trDelta[0] || a.trDelta[1] != b.trDelta[1] || a.trDelta[2] != b.trDelta[2];
	}

	// BG_EvaluateTrajectory for the trajectory types a missile flies on, in integer
	// micro-units: the server snaps a missile's trBase and trDelta to whole units, so
	// this is exact and x86 and aarch64 write the same numbers.
	static void EvaluateTrajectory(s32* result, const idTrajectoryBase& tr, s32 atTime)
	{
		s64 halfGravity = 0;
		switch((s32)tr.trType)
		{
			case UDT_TRACKS_TR_GRAVITY: halfGravity = UDT_TRACKS_GRAVITY / 2; break;
			case UDT_TRACKS_TR_GRAVITY_LOW: halfGravity = UDT_TRACKS_GRAVITY * 3 / 20; break;
			case UDT_TRACKS_TR_LINEAR:
			case UDT_TRACKS_TR_GRAVITY_FLOAT: break;
			default:
				CopyOrigin(result, tr.trBase);
				return;
		}
		const s64 dtMs = (s64)atTime - (s64)tr.trTime;
		for(u32 i = 0; i < 3; ++i)
		{
			s64 micro = (s64)floor(tr.trBase[i] + 0.5f) * 1000000 + (s64)floor(tr.trDelta[i] + 0.5f) * dtMs * 1000;
			if(i == 2)
			{
				micro -= halfGravity * dtMs * dtMs;
			}
			micro += 500000;
			result[i] = (s32)(micro >= 0 ? micro / 1000000 : -((-micro + 999999) / 1000000));
		}
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
	udtVMArray<TrackMissile> _missiles { "Tracks::Missiles" };
	udtVMArray<TrackMissilePoint> _missilePoints { "Tracks::MissilePoints" };
	udtVMArray<TrackPhase> _phases { "Tracks::Phases" };
	udtVMArray<TrackName> _names { "Tracks::Names" };
	udtString _map;
	s32 _rateMs;
	s32 _startTime;
	s32 _nextSampleTime;
	s32 _lastPhase;
	s32 _snapshotCount;
	bool _newSnapshot;
	s32 _activeMissile[MAX_GENTITIES]; // Index into _missiles of the missile flying as this entity, or -1.
	idTrajectoryBase _lastTrajectory[MAX_GENTITIES];
	s32 _lastSeenTime[MAX_GENTITIES];
	s32 _lastSeenSnapshot[MAX_GENTITIES];
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

	plugIn.Finish();
	if(!plugIn.Write(outputPath.GetPtr()))
	{
		fprintf(stderr, "Failed to write %s\n", outputPath.GetPtr());
		return false;
	}

	char message[512];
	sprintf(message, "Wrote %s: %u samples, %u kills, %u missiles", outputPath.GetPtr(), plugIn.GetSampleCount(), plugIn.GetKillCount(), plugIn.GetMissileCount());
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
