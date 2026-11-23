// SPDX-License-Identifier: MIT

// The parts of a set for a game on the SDK's sound archive player (NITRO-System's, with a sound archive, SDAT): the
// game's routines, the boot to the hook, the stub, the library's image and the run of a mini on ntdlib's DS.
//
// A set runs the game's code. Its ARM9 binary and the overlays that it keeps are stored decompressed. The start-up
// decompressor then returns at once whenever the game calls it: the entry code calls it for the ARM9 binary, and the
// SDK's overlay loader for each compressed overlay. Its body never runs, and the stub goes there. The first instruction
// of the sound library's update (NNS_SndMain) becomes a branch to the stub. The game calls the update for the first
// time once it has set up its sound heap and opened its archive. The stub never returns. It loads the sound's data into
// the game's sound heap and starts the sound with the game's routines. Then, once a frame, it waits for the vertical
// blank and runs the update: the instruction that the branch replaced, then the rest of the update.
//
// The library is a compact image (ntdlib/ntr/compact_image.h): the files that the boots (rip/boots.h) of the game and
// of the full image read before the hook, whole, and the sound archive cut to the data of the set's sounds. The cut
// keeps each kept file at its place in its card page. The library's boot then reads the card as the full image's boot
// does. The full image is the set's image with every file of the game kept whole. A mini stores its sound's number in
// the stub's word for it.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ntdlib/ntr/archive_player.h"
#include "ntdlib/ntr/code.h"
#include "ntdlib/ntr/game_profiles.h"
#include "ntdlib/ntr/machine.h"
#include "ntdlib/ntr/rom.h"
#include "ntdlib/ntr/snd/driver.h"
#include "ntdlib/ntr/sound.h"
#include "ntdlib/sdat/formats.h"
#include "xsflib/rip/length.h"

#include "rip/arm_code.h"
#include "rip/boots.h"

namespace twosf::rip
{

// An argument of a call that a stub makes: the sound's number, a constant, the word at an address of main memory when
// the stub makes the call, or the result of the call before. Only a call after the first of its list may take that
// result, as its first argument. An effect's calls (GameStart::effect_calls) also take the number of the sequence
// archive, the sequence's index in it, its bank, the sound heap, and the archive's number shifted left by `value` bits
// with the index in the bits below.
struct StubArgument
{
    enum class Kind : uint8_t
    {
        kSound,
        kConstant,
        kWord,
        kResult,
        kArchive,
        kEntry,
        kBank,
        kHeap,
        kPackedEntry,
    };

    friend bool operator==(const StubArgument&, const StubArgument&) = default;

    Kind kind = Kind::kConstant;
    uint32_t value = 0; // the constant, the word's address, or the packed entry's shift
};

// The most arguments that a stub's call passes: r0 to r3, then two on the stack.
inline constexpr std::size_t kStubArguments = 6;

// The arguments that a stub's call passes in registers, r0 to r3. The others go on the stack, the fifth at sp.
inline constexpr std::size_t kRegisterArguments = 4;

// A call that a stub makes: the routine, with bit 0 set for Thumb code, and its first `count` arguments.
struct StubCall
{
    friend bool operator==(const StubCall&, const StubCall&) = default;

    uint32_t routine = 0;
    std::array<StubArgument, kStubArguments> arguments = {};
    std::size_t count = 0;
};

// The sounds that a game starts with other calls than the rest: each range of numbers from `first` to `last`, and with
// `player` each sound whose record names that player.
struct NumberCalls
{
    friend bool operator==(const NumberCalls&, const NumberCalls&) = default;

    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    std::optional<uint32_t> player;
    std::vector<StubCall> calls;
};

// A state of a sequence that the game's code keeps set while the sequence plays, such as tracks that it silences or a
// tempo that it raises: the calls of the game's routines that set it. A set makes those calls after the start. The
// state with no suffix is the state in which the game starts the sequence, and the sequence's mini makes its calls.
// Each state with a suffix gets a mini of its own, named after the sequence's label and the suffix.
//
// The game's code can also change a state as the sequence plays, in its frames: a handler that fades the mix at a tick,
// or a scene that sets another state after a time. A set then makes `frame_calls` in each game frame after the start,
// and each of `timed_calls` in its game frame, after `frame_calls`. A game frame lasts `video_frames` frames of the
// screen, and the update runs at its end, as in the game's frame. The game frames count from 1, the first after the
// start's frame.
struct FrameCall
{
    friend bool operator==(const FrameCall&, const FrameCall&) = default;

    uint32_t frame = 0;
    StubCall call;
};

struct GameState
{
    friend bool operator==(const GameState&, const GameState&) = default;

    std::string label;
    std::string suffix;
    std::vector<StubCall> calls;
    std::vector<StubCall> frame_calls = {};
    std::vector<FrameCall> timed_calls = {};
    uint32_t video_frames = 1;
};

// Whether a set makes calls of `state` after the start, in the game's frames.
bool PlaysOn(const GameState& state);

// A game whose code starts its music with routines that aren't the SDK's alone: its music manager. A set calls them in
// place of the SDK's load and start, as the game calls them. Each list loads the sequence into the game's sound heap,
// starts it, and sets what the game sets for it, such as the channels that each track may take or the volume.
struct GameStart
{
    // The calls that start any number that `numbers` doesn't list, in order.
    std::vector<StubCall> calls;

    // The numbers that the game starts with other calls. The first list that holds the number counts.
    std::vector<NumberCalls> numbers;

    // The sequences' states, by label, in the order of the game's table.
    std::vector<GameState> states;

    // The calls that start a stream, in order, for a game that starts its streams through its stream manager: the
    // manager's set-up, then its start of the stream. Empty for a game whose streams a set starts with the SDK's start
    // (SdatCode::stream_start).
    std::vector<StubCall> stream_calls;

    // The calls that start an effect, a sequence of a sequence archive, in order: the loads of the archive and of the
    // sequence's bank into the sound heap, then the game's start of an effect. Empty for a game whose sets contain no
    // sequence of an archive.
    std::vector<StubCall> effect_calls;

    // Whether numbers with one record share a mini where their runs start them alike: the same commands around the
    // start, variables, channels, volume and priority, and the same capture units at the end (SameStart). Every other
    // number of a game in the table keeps a mini of its own (FirstOfRecord). Phantom Hourglass and Spirit Tracks: the
    // runs of 38 of their 40 groups of numbers with sound showed the same start for most groups.
    bool merge_same_starts = false;

    // The main function's first instruction after the sound's start-up, or 0. A set's library puts the main thread to
    // sleep there for good: the rest of the game's start-up, its frames and its requests of sounds never run.
    uint32_t park = 0;
};

// A sequence that the game plays only after it starts its sound again with another setting. Before that sequence, a set
// calls the game's start-up of its sound, `routine`, with `argument`, as the game does. The start-up makes the sound
// heap again at the same address.
struct SoundRestart
{
    uint32_t sequence = 0;
    uint32_t routine = 0;
    uint32_t argument = 0;
};

// The game's routines that a set calls. A routine in Thumb code has bit 0 set.
struct SdatCode
{
    uint32_t update = 0; // NNS_SndMain

    // The update's first instruction. The hook replaces it. In Thumb code, its first two halfwords, the first in the
    // low half: the hook is a blx of two halfwords.
    uint32_t update_first = 0;

    // NNS_SndArcPlayerStartSeq(handle, sequence), or with `start_ex` NNS_SndArcPlayerStartSeqEx(handle, player, bank,
    // priority, sequence). The stub calls the second with -1 for each of the player, the bank and the priority.
    uint32_t start = 0;
    bool start_ex = false;

    std::optional<uint32_t> loader; // NNS_SndArcLoadSeq(sequence, heap): nonzero once loaded
    uint32_t load = 0;              // without a loader: (sequence, 0xFF, heap, 1, null): 0 once loaded

    // The wait for a frame and its arguments: OS_WaitVBlankIntr(), OS_WaitIrq(1, 1), or for a game in FindSdatCode's
    // table its wait, such as OS_SleepThread(NULL).
    uint32_t wait = 0;
    std::vector<uint32_t> wait_arguments;

    // The game's changes of its code (GamePatch), from FindSdatCode's table. The set's binary contains them, and the
    // rippers boot the game with them (PatchedImage).
    std::vector<GamePatch> patches;

    // For a game in FindSdatCode's table whose sound heap no scan finds, the word of main memory that holds the heap's
    // address at the hook (SoundHeapAt).
    std::optional<uint32_t> heap_word;

    // NNS_SndArcStrmStart(handle, stream, offset): the start of one of the archive's streams (STRM). Nothing when the
    // search doesn't find exactly one, when the stub that calls it doesn't fit, or when the table of games gives the
    // calls of the game's stream manager (GameStart::stream_calls). A set then makes those calls in its place. A set
    // contains streams with either (StartsStreams). With neither, `no_stream_start` says why.
    std::optional<uint32_t> stream_start;
    std::string no_stream_start;

    // The game's starts, for a game in FindSdatCode's table. The set then calls neither `start` nor a load.
    std::optional<GameStart> game_start;

    // For a game in FindSdatCode's table of restarts, the sequence that the stub starts only after the game's sound
    // starts again.
    std::optional<SoundRestart> restart;

    // For a game in the table of restarts whose start-up doesn't start with the table's words, the restart's sequence.
    // A set leaves it out: the sequence doesn't fit the heap that the boot makes.
    std::optional<uint32_t> left_out;

    // The decompressor's body, from `stub` up to `stub_end`. A set never runs it.
    uint32_t stub = 0;
    uint32_t stub_end = 0;

    // For a game in the table of games, the size of each start block of its sets (StartBlockBytes), once the rip has
    // read its sound archive. With 0, a start block takes the size of the largest that any list and state of the table
    // can make.
    uint32_t start_block = 0;
};

// The game's sound archive: the file of the image that holds it at its start, with sequences.
struct SoundArchive
{
    uint32_t file_id = 0;
    ntdlib::sdat::Archive archive;
};

// Finds the routines in `rom`'s ARM9 binary (ntdlib/ntr/sound_routines.h, and FindStreamStarts in rip/stream_set.h). A
// game in the table of games starts its music through its music manager (GameStart), with the table's calls in place of
// the SDK's load and start. The table may also give the game's wait, patches of its code, the word of its sound heap
// and the calls of its stream manager: Pokémon Black 2 sets up the SDK's stream player only in its title screen, after
// the hook. The table has Pokémon Mystery Dungeon: Blue Rescue Team (USA, APHE), Pokémon Black, White, Black 2 and
// White 2 (USA, IRBO, IRAO, IREO and IRDO), Pokémon HeartGold and SoulSilver (USA, IPKE and IPGE), Pokémon Diamond,
// Pearl and Platinum (USA, ADAE, APAE and CPUE), Mario Kart DS (USA, AMCE), The Legend of Zelda: Spirit Tracks (USA and
// Europe, BKIE and BKIP, each in revisions 0 and 1) and Phantom Hourglass (USA, AZEE). A game in the table of restarts
// takes its restart from that table (LookUpRestart): Fire Emblem: Shin Monshou no Nazo (Japan, VI2J). Returns nothing,
// with `error` set, when the game's code doesn't contain an instruction that the table of games expects, when the
// search doesn't give exactly one update, and outside the table of games one start, one load and one wait, when it
// finds no start-up decompressor, when the update or the decompressor is outside the binary's static part, when the
// update's first instruction can't run elsewhere (Relocatable, RelocatableThumb), or when the stub doesn't fit in the
// decompressor's body.
std::optional<SdatCode> FindSdatCode(const ntdlib::ntr::Rom& rom, std::string& error);

// Why the stub of a set of `code` doesn't fit in the start-up decompressor's body: the bytes of each, or nothing when
// it fits. A game in the table of games is checked with its start blocks (SdatCode::start_block).
std::optional<std::string> StubSizeProblem(const SdatCode& code);

// Whether a set of the game contains streams: its stub starts one with the calls of the game's stream manager
// (GameStart::stream_calls) or with the SDK's start (SdatCode::stream_start).
bool StartsStreams(const SdatCode& code);

// Whether `arm9`, the ARM9 binary decompressed, contains every instruction word that an entry of the table of games
// expects for the game with `game_code`. The table can have an entry for each revision of a game. False for a game
// outside the table, and for a game whose entries expect a word of an overlay.
bool MatchesGameStart(const ntdlib::ntr::LoadedCode& arm9, std::string_view game_code);

// The calls with which a set of the game with `game_code` starts `sound`, whose record names `player`: the calls of the
// first list of the table's numbers that holds the sound, or else the game's calls (GameStart). Empty for a game
// outside the table of games.
std::vector<StubCall> StartCallsOf(std::string_view game_code, uint32_t sound, std::optional<uint32_t> player);

// What the table of restarts gives a game: the restart that the stub makes, or the sequence that the set leaves out.
// Both are empty for a game outside the table.
struct RestartLookup
{
    std::optional<SoundRestart> restart;
    std::optional<uint32_t> left_out;
};

// The table of restarts for the game with `game_code` in `arm9`, the ARM9 binary decompressed. Nothing for a game
// outside the table. For a listed game, the restart when the start-up starts with the table's two words, and otherwise
// the restart's sequence as `left_out`.
RestartLookup LookUpRestart(const ntdlib::ntr::LoadedCode& arm9, std::string_view game_code);

// Whether the ARM instruction `word` runs the same at another address: a push of registers other than the pc onto the
// stack (stmfd sp!, or str rd, [sp, #-4]!), or an instruction of data processing whose registers aren't the pc. The
// hook moves the update's first instruction into the stub.
bool Relocatable(uint32_t word);

// The ARM instructions that do what the Thumb instructions `halves` do, the first in the low half: push {list, lr},
// movs rd, #imm and adds rd, rm, #0, in either place. Nothing for any other instruction. The Thumb hook replaces the
// update's first two halfwords, and the stub runs them in ARM state.
std::optional<std::array<uint32_t, 2>> RelocatableThumb(uint32_t halves);

// The word that the hook writes at the update's first instruction: a branch to the stub, or in Thumb code a blx to the
// stub of two halfwords, the first in the low half. The blx enters ARM state.
uint32_t HookWord(const SdatCode& code);

// `rom`'s image as a DS runs it. A DS runs a DSi-enhanced image as a DS image: the copy has the unit code of a DS image
// and its header's CRC-16 again. Any other image is copied unchanged.
std::vector<uint8_t> DsImage(const ntdlib::ntr::Rom& rom);

// The first of `files`, file IDs of `rom`, that contains a sound archive at its start with at least one sequence.
// Nothing when none does.
std::optional<SoundArchive> FindSoundArchive(const ntdlib::ntr::Rom& rom, const std::set<uint32_t>& files);

// The sequences outside the sequence archives that count as music, by index: the music of ntdlib::ntr::rip::IsMusic for
// the game with `game_code` in an archive with names. IsMusic counts the players named for music, the fields and the
// fanfares, such as PLAYER_BGM, PLAYER_FIELD and PLAYER_ME in the Pokémon games. For the game, the table of music
// prefixes (`kMusicPrefixes`) may add the sequences whose names start with a prefix: the fanfares FF_ of both Fire
// Emblem DS games. When none counts, or the archive has no names, every sequence outside the sequence archives does.
// The SDK's tools put most sound effects in sequence archives.
std::set<uint32_t> MusicSequences(const ntdlib::sdat::Archive& archive, std::string_view game_code);

// What a stub starts: a sequence, or a stream of the archive, with `code.stream_start` or with the calls of the game's
// stream manager (GameStart::stream_calls), or a stream that the game plays from a file of its own through its play
// routine (rip/game_streams.h).
enum class SoundKind : uint8_t
{
    kSequence,
    kStream,
    kGameStream,
};

// `numbers` are sounds of `kind`, in the order of the set: sequences outside the sequence archives, or streams. For
// each of them, the first of `numbers` with the same record. For a sequence, that's the same sequence file, bank,
// volume, channel priority, player priority, player and reserved bytes. For a stream, it's the same file, volume,
// player priority, player, flags and reserved bytes. The name doesn't count. The SDK's start routines read only the
// record. A set therefore plays the numbers of one record the same, and only the first gets a mini. A number without a
// record maps to itself. So does every sequence of a game in the table of games (`code.game_start`): a set starts its
// sequences through the game's routines, not through the SDK's start alone, and those routines may treat two numbers of
// one record differently. A set starts a stream with the SDK's start, except in a game whose table entry gives the
// calls of its stream manager (GameStart::stream_calls): every stream of such a game maps to itself too.
std::vector<uint32_t> FirstOfRecord(const SdatCode& code, const ntdlib::sdat::Archive& archive,
                                    std::span<const uint32_t> numbers, SoundKind kind = SoundKind::kSequence);

// The limit of the length analysis of sequence `sequence`: the long limit for music (`music`, from MusicSequences), the
// default limit for the rest (xsflib::rip::Limit).
xsflib::rip::Limit AnalysisLimit(const std::set<uint32_t>& music, uint32_t sequence);

// The game's sound heap (NNSSndHeapHandle): the heap whose frame heap, with the signature FRMH 0x10 bytes after the
// heap's start, contains the others. A frame heap counts only when its start, its head, its tail and its end are in
// main memory in that order. Where no heap contains the others, the sound heap is the one heap whose used part, from
// its start to its head, contains a sound archive's information block (INFO). NNS_SndArcInit loads the block into the
// heap that the game gives it. Fire Emblem: Shadow Dragon (USA, YFEE) makes two sound heaps side by side (0x02015DC8)
// and gives the archive the first. `free` gives the frame heap's free bytes.
struct SoundHeap
{
    friend bool operator==(const SoundHeap&, const SoundHeap&) = default;

    uint32_t address = 0;
    uint32_t free = 0;
};

// The sound heap in `memory`, the DS's main memory from 0x02000000. Nothing when it contains no frame heap, or when no
// heap contains the others and the information block isn't in exactly one.
std::optional<SoundHeap> FindSoundHeap(std::span<const uint8_t> memory);

// The sound heap whose address the word at `word` of `memory` holds. Nothing when that address doesn't hold a frame
// heap whose start, head, tail and end are in main memory in that order. Mario Kart DS (USA) makes several frame heaps,
// and none contains the others: its sound heap is the word at 0x0217D110.
std::optional<SoundHeap> SoundHeapAt(std::span<const uint8_t> memory, uint32_t word);

// What a boot of an image gives at its first call of the update: the time, the ARM9's CPSR, the files that the card's
// reads reached, and the sound heap.
struct HookState
{
    bool reached = false;
    uint64_t clock = 0; // the machine's clock, in cycles of the system clock
    double seconds = 0;
    uint32_t cpsr = 0;
    std::set<uint32_t> files;
    std::vector<std::pair<uint32_t, uint32_t>> reads; // each read's start and end in the image
    std::optional<SoundHeap> heap;
};

// Boots `rom` on ntdlib's DS with `backup` in its card until the ARM9 reaches `update`, for at most 30 s. The sound
// heap comes from the word `heap_word` where one is given (SoundHeapAt), and from the scan otherwise (FindSoundHeap).
HookState BootToHook(const ntdlib::ntr::Rom& rom, uint32_t update, const ntdlib::ntr::BackupMemory& backup = {},
                     std::optional<uint32_t> heap_word = std::nullopt);

// The first boot on which the library's boot differs from the full image's (rip::DifferentBoot: it misses the hook,
// reaches it at another clock or reads other files), makes another count of card reads, or reaches the hook with the
// sound heap elsewhere or with other bytes free, from `full` and `library`, the boots of each in the order of
// BootBackups. Nothing when every boot matches. The cut archive's page layout (CutArchive) lets a good library match
// exactly. The count of reads sees a library that reads the card differently at the same clock.
std::optional<std::size_t> UnlikeHookBoot(const std::vector<HookState>& full, const std::vector<HookState>& library);

// The state of the first of `boots`, with the files and the reads of every boot. A set keeps the files that any boot
// reads before the hook.
HookState MergedHook(std::span<const HookState> boots);

// Why the game's state at the hook doesn't let a stub run: no hook, no heap, an interrupt's mode or interrupts off.
// Nothing when it does.
std::optional<std::string> HookProblem(const HookState& hook);

// Why a boot's state at the hook doesn't let the stub run with the sound heap `heap`, the heap of the game's boot with
// no backup memory: a HookProblem, or the sound heap at another address. The stub contains that address. The rippers
// check each boot of the game and of the full image with it. Nothing when the stub can run.
std::optional<std::string> HookProblem(const HookState& hook, const std::optional<SoundHeap>& heap);

// The IDs of the files of `archive` that the reads of `hook` reach. `archive` starts at `start` in the image.
std::set<uint32_t> ArchiveFilesRead(const HookState& hook, uint32_t start, const ntdlib::sdat::Archive& archive);

// What the boots of an image read before the hook: the image's files, and the files of the sound archive.
struct BootFiles
{
    friend bool operator==(const BootFiles&, const BootFiles&) = default;

    std::set<uint32_t> files;
    std::set<uint32_t> archive_files;
};

// What `boots`, boots of `rom` (MergedHook), read before the hook, with the sound archive `found`. `found` gives the
// archive's file ID, and `rom` gives where the archive starts.
BootFiles FilesRead(const ntdlib::ntr::Rom& rom, const SoundArchive& found, std::span<const HookState> boots);

// What a library keeps of the image and of the sound archive: what any boot of the game reads before the hook, `game`,
// and what any boot of the full image reads, `full`. The full image is the set's image with every file kept whole.
BootFiles LibraryRead(const BootFiles& game, const BootFiles& full);

// The offset of each file of `archive`, by ID, in a cut that keeps the files `kept` at their places in their card
// pages. The cut keeps each page of `archive` that contains a byte in front of the lowest file offset (the header, the
// tables and the FILE block's header) or a byte of a file of `kept`, in order, and drops the others. A file's offset is
// kCardPage times the count of kept pages in front of its first page, plus its offset in `archive` modulo kCardPage. A
// read of a kept file then reaches the same places in the same count of pages as in `archive`.
std::vector<uint32_t> PageOffsets(const ntdlib::sdat::Archive& archive, const std::set<uint32_t>& kept);

// `archive` cut to the data that `sequences` play (ntdlib::ntr::rip::KeepSequenceData), with the files `extra` kept
// too. Each other file of `reserved` keeps its size and contains zeros: a stream's library fills it. A file of both
// `extra` and `reserved` keeps its data, because the boot reads it before any stream's library can fill it. Every other
// file is empty. The tables are the archive's. The files with bytes keep their places in their card pages
// (PageOffsets). A boot then reads the cut page for page as it reads the whole archive. Each file with bytes is read
// back from the cut and compared. Nothing when the cut fails or a file differs.
std::optional<std::vector<uint8_t>> CutArchive(const ntdlib::sdat::Archive& archive,
                                               std::span<const ntdlib::ntr::SequenceRef> sequences,
                                               const std::set<uint32_t>& extra,
                                               const std::set<uint32_t>& reserved = {});

// The sound archive of a set's library: the cut (CutArchive) and the files that it reserves, or the whole archive with
// no reserved file when the cut fails. The whole archive contains every stream file. A stream mini then uses no stream
// library.
struct SetArchive
{
    std::vector<uint8_t> bytes;
    std::set<uint32_t> reserved;
    bool whole = false;
};

SetArchive MakeSetArchive(const ntdlib::sdat::Archive& archive, std::span<const ntdlib::ntr::SequenceRef> sequences,
                          const std::set<uint32_t>& extra, const std::set<uint32_t>& reserved);

// The stub at `code.stub`, and where its word for the sound's number, `sound`, is from its start. With the game's
// starts, the stub starts with the sound's start block (StartBlock, with `player`), and `heap` goes unused. Without
// them, the stub loads the sequence into `heap` and starts it with the SDK's start, and with a restart it first calls
// the game's start-up of its sound for the restart's sequence. A stream's stub calls `code.stream_start` with the
// handle, the stream and 0, and `heap` goes unused. With the calls of the game's stream manager
// (GameStart::stream_calls), a stream's stub makes them in place of the SDK's start, as direct calls with the stream's
// number as a constant, and `sound_offset` goes unused: a stream's mini is its whole stub. Each frame, the stub calls
// the wait with its arguments, then the update. Throws std::logic_error for a stream's stub of a game whose set
// contains no streams (StartsStreams).
struct SdatStub
{
    std::vector<uint8_t> bytes;
    uint32_t sound_offset = 0;
    uint32_t sound_bytes = 4; // the bytes of a mini: the sound's word, or a game's start block (StartBlock)
};

SdatStub BuildSdatStub(const SdatCode& code, uint32_t heap, uint32_t sound, SoundKind kind = SoundKind::kSequence,
                       std::optional<uint32_t> player = std::nullopt);

// The stub at `code.stub` that calls `routine` with `argument` once, then runs the frames as BuildSdatStub's do. A mini
// of such a stub is the whole stub. With `frame_routine`, each frame calls that routine of the game after the wait, in
// place of the update. The game's routine calls the update, and the hook brings the call back to the stub. The stub
// runs the update and returns.
SdatStub BuildCallStub(const SdatCode& code, uint32_t routine, uint32_t argument,
                       std::optional<uint32_t> frame_routine = std::nullopt);

// For a game in the table of games, the stub's start block for `sound`, whose record names `player`, in the state
// `state` (GameState, or the sequence's state with no suffix when `state` is null): the calls of its list (the first
// list of `code.game_start->numbers` that holds the sound, or else `calls`), then the state's calls, as direct calls
// with the sound's number as a constant, then a branch to the frames, then the block's literals, padded with zeros to
// the largest block of the game. A sequence's mini of such a game is its block, at the stub's start. The table's lists
// don't fit in the decompressor's body side by side: Mario Kart DS's would take about 190 bytes of 160.
std::vector<uint8_t> StartBlock(const SdatCode& code, uint32_t sound, std::optional<uint32_t> player,
                                const GameState* state = nullptr);

// The size of every start block of a set of a game in the table of games whose sound archive is `archive`: the largest
// block that any list of the table makes with any of its states, and the effects' block (EffectBlock), where that fits
// beside the frames in the decompressor's body. Otherwise the largest block that the archive's sequences make
// (StartBlock), alone and with each of their states (StatesOf), and the effects' block. Each number and value is
// measured as a literal.
uint32_t StartBlockBytes(const SdatCode& code, const ntdlib::sdat::Archive& archive);

// The start block of an effect of a game in the table of games: the game's calls (GameStart::effect_calls) for
// `sequence`, a sequence of a sequence archive of `archive`, with the sound heap `heap`, padded to the size of every
// start block. Throws std::logic_error for a game without the calls or a sequence outside the archives.
std::vector<uint8_t> EffectBlock(const SdatCode& code, const ntdlib::sdat::Archive& archive,
                                 const ntdlib::ntr::SequenceRef& sequence, uint32_t heap);

// The states of the sequence `label` of a game in the table of games (GameStart::states), in the table's order. The
// state with no suffix, if any, comes first. Empty for a game outside the table.
std::vector<const GameState*> StatesOf(const SdatCode& code, std::string_view label);

// The library's image of `rom`: the ARM9 binary decompressed with `code.patches`, `stub` and the hook, and the files
// `files`. The hook is a branch to the stub at the update's first instruction, or in Thumb code a blx at its first two
// halfwords. The SDK's overlay loader decompresses an overlay with the start-up decompressor. A kept overlay that the
// image compresses therefore goes in decompressed, and its entry in the overlay table loses its compression flags.
// `sound_word` gives the offset in the image of the stub's word for the sound's number, and `stub` the stub's offset.
struct SdatSet
{
    std::vector<uint8_t> image;
    uint32_t stub = 0;
    uint32_t sound_word = 0;
};

SdatSet MakeSdatSet(const ntdlib::ntr::Rom& rom, const SdatCode& code, const SdatStub& stub,
                    const std::map<uint32_t, std::vector<uint8_t>>& files);

// The list of the driver's commands that the ARM9 sends with `word`, a word of the FIFO to the ARM7, read from
// `memory`, the DS's main memory. Nothing for a word of another kind.
std::optional<ntdlib::ntr::snd::CommandList> SoundCommands(std::span<const uint8_t> memory, uint32_t word);

// How a command `id` in a list of the driver's commands after a sequence's start changes what the sequence plays: it
// starts, prepares or stops a sequence, sets a variable, or sets a volume, a mute or a parameter of a player or a
// track. Nothing for a command that changes none of them.
std::optional<std::string> Interference(ntdlib::ntr::snd::CommandId id);

// Makes `machine`, the machine that runs `rom`, call `fail` for each card read from `hook_clock` on that reaches none
// of the bytes that the set keeps: every file with bytes other than the sound archive, the file `archive_file`, the
// archive's header and tables, and each of the archive's files with bytes but the files `left_out`, and a DSi-only
// image's hash tables. A file that the set reserves for a stream's library contains zeros in the set's image, and the
// run of any other mini leaves it out. A DSi-only image keeps each file's place and zeroes the files that it leaves
// out, but for their bytes in a sector of the digest that a kept file reaches. Such a file counts as left out when its
// bytes in the sectors that no other file reaches are zeros. In a DSi-only image, each range widens to whole sectors of
// the digest: the game reads and checks whole sectors.
void GuardCardReads(ntdlib::ntr::Machine& machine, const ntdlib::ntr::Rom& rom, uint32_t archive_file,
                    const ntdlib::sdat::Archive& archive, const std::set<uint32_t>& left_out, uint64_t hook_clock,
                    std::function<void(std::string)> fail);

// What a mini's run on ntdlib's DS gives: its boot, then the sound's start. `start_ms` is the time from the boot to the
// list of the driver's commands that starts the sound. `variables` contains the global variables that the commands up
// to then set, and the local variables of the sequence's player from its prepare to the end of the list that starts it.
// `sounded` says whether the sound unit's output changed after the start. `captures_run` is whether the capture units
// run at the end of the run, as the SDK's reverb and output effect run them (the busy bits of SNDCAP0CNT and
// SNDCAP1CNT). `error` says why the run doesn't count.
//
// `channels` gives the channels that the game gives every track of the sequence's player (the driver's command 9 with
// every track), as the SDK sends the channels of the archive's player or the game's. `volume` and `priority` are the
// player's volume and priority before the start, as the SDK's start sends them. `commands` contains the other commands
// on the sequence's player from its prepare to the end of the list that starts it: what the game's code sets around the
// start, such as the tracks' volumes, a tempo or some tracks' channels. They address the driver's player 0 here, as a
// model plays the sequence. `locked` gives the channels that the game's code has locked by the end of that list
// (NNS_SndLockChannel), as a stream's preparation locks them.
//
// A run of a state that plays on (PlaysOn) also records `later`: the commands of each later list that set the
// sequence's player, addressed to player 0, and its locks and unlocks of channels, with the system clocks from the list
// that starts the sequence. Its game frames last `video_frames` frames of the screen, and `played_clocks` is the system
// clocks that the run played on after the start.
struct LaterList
{
    friend bool operator==(const LaterList&, const LaterList&) = default;

    uint64_t clocks = 0;
    ntdlib::ntr::snd::CommandList commands;
};

struct MiniRun
{
    std::optional<std::string> error;
    bool not_started = false; // the run fails only because the sequence doesn't start in time
    double start_ms = 0;
    ntdlib::ntr::Variables variables;
    bool sounded = false;
    bool captures_run = false;
    std::optional<uint16_t> channels;
    int16_t volume = 0;
    int16_t priority = 0;
    ntdlib::ntr::snd::CommandList commands;
    uint16_t locked = 0;
    std::vector<LaterList> later;
    uint32_t video_frames = 0;
    uint64_t played_clocks = 0;
};

// Follows the driver's command lists of a mini's run up to its sequence's start, the start command on a player with a
// prepared sequence. A start command both prepares and starts. A stop of a player drops its prepare: a game's start can
// stop a sequence that the boot queued. Records in `run` the global variables that the lists set, and what they set on
// the sequence's player from its prepare to the end of the list that starts it: its local variables, MiniRun::channels,
// MiniRun::volume and MiniRun::priority before the start, and MiniRun::commands. The locks and unlocks of channels up
// to the end of that list give MiniRun::locked.
class StartFollower
{
public:
    explicit StartFollower(MiniRun& run) : run_(run)
    {
    }

    // Takes the next list. Returns the prepare of the sequence when the list starts it. Call it only up to that list.
    std::optional<ntdlib::ntr::snd::Command> Take(const ntdlib::ntr::snd::CommandList& list);

    // The driver's player of the sequence, once it starts.
    uint32_t Player() const
    {
        return player_;
    }

private:
    struct Prepared
    {
        ntdlib::ntr::snd::Command prepare;
        ntdlib::ntr::snd::CommandList commands;
    };

    void Record(const ntdlib::ntr::snd::Command& command, bool started);

    MiniRun& run_;
    std::map<uint32_t, Prepared> pending_;
    uint32_t player_ = 0;
};

// The time that a mini's run plays on after its sequence starts, and after the last change of a state that plays on.
constexpr double kMiniPlaySeconds = 5.0;

// The time that the run of a state that plays on plays on after its sequence starts: it covers the intros that the
// table's states play, and kMiniPlaySeconds after the last change.
constexpr double kPlayOnSeconds = 30.0;

// The game frames after a start that a profile's start covers: the game's code can set the start's values in the game
// frames after the start, such as Spirit Tracks' mixer in the next frame. A profile's start sends them with the start.
constexpr uint64_t kStartFrames = 2;

// The settings of the length analysis of a mini: what the set plays. They contain the variables that `run` sent, and
// the channels that its start gave the tracks of the archive's player `player` where they differ from the archive's
// `archive_channels`. When the run ends with the capture units running, they also contain the reverb or output effect
// of `profile`, the game's profile (ntdlib::ntr::GameProfileOf). A game can start its reverb in a function that the set
// doesn't run. The set then plays no echo. The commands around the start (MiniRun::commands) go to the analysis after
// the start (ntdlib::ntr::MeasureSequence).
ntdlib::ntr::GameSettings LengthSettings(const MiniRun& run, const ntdlib::ntr::GameSettings& profile, uint32_t player,
                                         uint16_t archive_channels);

// How the start of `run` differs from what the game's profile gives the sequence: `profile`, followed by the
// arrangement `arrangement` when one is given. The profile is the check of a set that starts the sequence through the
// game's code. Both sides become the values that they leave set on the sequence's player: the channels of every track
// and of single tracks, the player's volume and tempo, and each track's volume, pitch, pan and mute. A value that a
// side leaves at the start's value counts as unset: the archive's channels `archive_channels`, the player's volume and
// priority before the start (MiniRun::volume and MiniRun::priority), a tempo of 256, and 0 for the others. The player's
// local variables count too: the profile's, then the arrangement's, with the archive's -1 for a variable that a side
// leaves alone. Channels locked at the start (MiniRun::locked, and the profile's locks in frame 0) count as channels
// that no track of the player has. The run's locks of channels that no lock of the profile names count on both sides.
// Such a lock is the game's state outside the profile, such as the channels that a game locks as it boots. A run whose
// capture units run at the end requires the profile's reverb or output effect, and the profile's reverb requires them.
// The run's side of a run with later lists is its start as LaterDifferences finds it. Returns a line for each
// difference, empty when they agree.
std::vector<std::string> ProfileDifferences(const MiniRun& run, const ntdlib::ntr::GameProfile& profile,
                                            const ntdlib::ntr::ProfileArrangement* arrangement,
                                            uint16_t archive_channels);

// How the later lists of `run` (MiniRun::later) differ from the lists of the arrangement `arrangement` that the game's
// code sends once the sequence has run some ticks (ntdlib::ntr::ProfileArrangement::tick_commands). Each side becomes
// the values that it leaves set on the sequence's player, as ProfileDifferences gives them, after each list that
// changes them. The run's start counts from the first game frame up to kStartFrames whose values equal the profile's
// start. Then each of the run's changes must equal the profile's next change, in the order of their ticks: a change in
// the n-th list at tick 0 comes in game frame n, and each other change of a list comes as many game frames after that
// list's first change as the profile gives. The last change comes at least kMiniPlaySeconds before the run's end. The
// run's later locks and unlocks of channels must be the profile's after frame 0 (GameProfile::channel_locks), each in
// the game frame nearest the profile's frame of the driver. Returns a line for each difference, empty when they agree
// or when neither side has later lists or locks.
std::vector<std::string> LaterDifferences(const MiniRun& run, const ntdlib::ntr::GameProfile& profile,
                                          const ntdlib::ntr::ProfileArrangement* arrangement,
                                          uint16_t archive_channels);

// The later lists of `run` as lists that the game's code sends from the start (ntdlib::ntr::TickCommands at tick 0),
// for the length analysis: the commands of the n-th game frame after the start in the n-th list, up to the last list
// that changes a value or locks or unlocks channels. Nothing for a run without later lists.
std::vector<ntdlib::ntr::TickCommands> LaterTickCommands(const MiniRun& run);

// Whether `run` ends with the capture units running and `settings` contain no reverb and no output effect for the model
// to play. The analysis then leaves out the echo, and the ripper warns.
bool EchoLeftOut(const MiniRun& run, const ntdlib::ntr::GameSettings& settings);

// The step of a mini's run between its checks: a frame.
constexpr uint64_t kRunStep = ntdlib::ntr::kSystemClockHz / 60;

// Boots `image`, a set's image with a mini in place, with `backup` in its card, until `sequence` of the set's sound
// archive, the file `archive_file`, starts, for at most `seconds`. Then it plays on for kMiniPlaySeconds. A mini of a
// state that plays on (PlaysOn), whose game frames last `video_frames` frames of the screen, plays on for
// kPlayOnSeconds and records its later lists (MiniRun::later): LaterDifferences checks them. The run fails:
// - when the sequence doesn't start in time (MiniRun::not_started, unless the run failed earlier);
// - when the command that prepares the sequence doesn't name the sequence's bytes and its bank's in main memory, or
//   when a slot of the bank that names a wave archive has no link to main memory once the load has written the links;
// - when a later list of the driver's commands prepares, starts or stops a sequence, sets a variable, or sets a
//   volume, a mute or a parameter of a player or a track: the lengths assume that the game leaves the sequence alone.
//   A state that plays on may set the sequence's player;
// - when a card read after `hook_clock`, the clock of the hook (HookState) on the boot with `backup`, reaches none of
//   the image's files and none of the archive's, or one of the files `reserved` (CutArchive). Those files contain
//   zeros.
MiniRun RunMini(std::span<const uint8_t> image, uint64_t hook_clock, uint32_t archive_file,
                const ntdlib::ntr::SequenceRef& sequence, const std::set<uint32_t>& reserved, double seconds,
                const ntdlib::ntr::BackupMemory& backup = {}, uint32_t video_frames = 0);

// RunMini for an image with no files whose sound archive `archive` is in main memory, such as a download-play program's
// (rip/program_set.h). Any card read after `hook_clock` fails the run.
MiniRun RunMemoryMini(std::span<const uint8_t> image, uint64_t hook_clock, const ntdlib::sdat::Archive& archive,
                      const ntdlib::ntr::SequenceRef& sequence, double seconds,
                      const ntdlib::ntr::BackupMemory& backup = {});

// The checks of a mini's runs, `runs`, one on each boot in the order of BootBackups, for BootsError. A run with a
// backup memory also fails when the driver's commands set other variables than on the boot with none. The length comes
// from the boot with none.
std::vector<BootRun> MiniChecks(std::span<const MiniRun> runs);

// Whether two runs start their sequences alike: the same commands around the start, the same variables, channels,
// locked channels, volume and priority, the same later lists, and the capture units in the same state at the end.
bool SameStart(const MiniRun& a, const MiniRun& b);

// For a game whose table entry merges numbers that start alike (GameStart::merge_same_starts): for each of `numbers`,
// sequences in the order of the set, the first of `numbers` with the same record (as FirstOfRecord compares it) whose
// run in `runs` starts it alike (SameStart), or else the number itself. `runs` gives the run of each number, or null
// for a number that keeps its mini, such as a number that failed. The ripper compares the minis of one state's suffix.
std::vector<uint32_t> FirstOfSameStart(const ntdlib::sdat::Archive& archive, std::span<const uint32_t> numbers,
                                       std::span<const MiniRun* const> runs);

// Whether a mini whose runs fail is left out of the set, as a sequence that plays no notes. That holds only for a game
// in the table of games (`code.game_start`), only when each of the mini's runs `runs` fails because the sequence
// doesn't start (MiniRun::not_started), and only when `timing`, the sequence's measure with no settings
// (ntdlib::ntr::MeasureSequence), is silent. A game's music manager can refuse a record that names no sound, such as
// Platinum's SEQ_BGM_END (file 0, volume 0). Any other failure stops the rip.
bool LeftOutUnstarted(const SdatCode& code, std::span<const MiniRun> runs,
                      const std::optional<xsflib::rip::Timing>& timing);

} // namespace twosf::rip
