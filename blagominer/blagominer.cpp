﻿#include "stdafx.h"
#include "blagominer.h"

#include "blagominer_meta.h"
#include "InstructionSet.h"
#include "bfs.h"
#include "network.h"
#include "shabal.h"
#include "filemonitor.h"
#include "updateChecker.h"
#include "elevate.h"
#include "volume_ntfs.h"
#include "inout.h"
#include "inout.nogui.h"
#include "loggerCsv.h"

#include <curl/curl.h>
#include "hexstring.h"

bool exitHandled = false;

// Initialize static member data
std::map<size_t, std::thread> worker;	        // worker threads

std::thread updateChecker;

const InstructionSet::InstructionSet_Internal InstructionSet::CPU_Rep;

t_logging loggingConfig;
t_gui guiConfig;

std::vector<std::shared_ptr<t_coin_info>> allcoins;
std::vector<std::shared_ptr<t_coin_info>> coins;

t_testmode_config testmodeConfig;

std::vector<wchar_t> p_minerPath; // TODO: use std::wstring
unsigned long long total_size = 0;
volatile bool stopThreads = false;				// only applicable to WORKER threads
bool use_wakeup = false;						// wakeup HDDs if true
unsigned int hddWakeUpTimer = 180;              // HDD wakeup timer in seconds

bool use_debug = false;

bool proxyOnly = false;

 std::map<size_t, t_worker_progress> worker_progress;

std::vector<std::wstring> paths_dir; // paths

sph_shabal_context  local_32;

std::unique_ptr<IUserInterface> gui;

void init_mining_info(std::shared_ptr<t_coin_info> coin, std::wstring name, size_t priority, unsigned long long poc2start)
{
	coin->coinname = name;
	coin->locks = std::make_shared<t_locks>();
	coin->mining = std::make_shared<t_mining_info>();

	coin->mining->miner_mode = 1;
	coin->mining->priority = priority;
	coin->mining->state = MiningState::DONE;
	coin->mining->baseTarget = 0;
	coin->mining->height = 0;
	coin->mining->deadline = 0;
	coin->mining->scoop = 0;
	coin->mining->enable = true;
	coin->mining->my_target_deadline = MAXDWORD; // 4294967295;
	coin->mining->POC2StartBlock = poc2start;
	coin->mining->enableDiskcoinGensigs = false;
	coin->mining->dirs = std::vector<std::shared_ptr<t_directory_info>>();
}

void init_logging_config() {
	loggingConfig.enableLogging = true;
	loggingConfig.enableCsv = true;
	loggingConfig.logAllGetMiningInfos = false;
}

void init_gui_config() {
	guiConfig.disableGui = false;
}

void Gui_init()
{
	if (guiConfig.disableGui)
		gui = std::make_unique<Output_PlainText>();
	else
		gui = std::make_unique<Output_Curses>(guiConfig.size_x, guiConfig.size_y, guiConfig.lockWindowSize);
}

void resetDirs(std::shared_ptr<t_coin_info> coinInfo) {
	Log(L"Resetting directories for %s.", coinInfo->coinname.c_str());
	for (auto& directory : coinInfo->mining->dirs) {
		directory->done = false;
		for (auto& file : directory->files) {
			file.done = false;
		}
	}
}

bool allDirsDone(std::shared_ptr<t_coin_info> coinInfo) {
	for (auto& directory : coinInfo->mining->dirs) {
		if (!directory->done) {
			return false;
		}
	}
	return true;
}

struct OrderingByMiningPriority
{
	inline bool operator() (const t_coin_info& struct1, const t_coin_info& struct2)
	{
		return (struct1.mining->priority < struct2.mining->priority);
	}
	inline bool operator() (const std::shared_ptr<t_coin_info>& struct1, const std::shared_ptr<t_coin_info>& struct2)
	{
		return (struct1->mining->priority < struct2->mining->priority);
	}
};

void loadCoinConfig(DocumentUTF16LE const & document, std::wstring section, std::shared_ptr<t_coin_info> coin)
{
	if (document.HasMember(section.c_str()) && document[section.c_str()].IsObject())
	{
		Log(L"### Loading configuration for %s ###", section.c_str());

		const ValueUTF16LE& settings = document[section.c_str()];

		if (settings.HasMember(L"EnableMining") && (settings[L"EnableMining"].IsBool()))	coin->mining->enable = settings[L"EnableMining"].GetBool();
		Log(L"EnableMining: %d", coin->mining->enable);

		if (settings.HasMember(L"EnableProxy") && (settings[L"EnableProxy"].IsBool())) coin->network->enable_proxy = settings[L"EnableProxy"].GetBool();
		Log(L"EnableProxy: %d", coin->network->enable_proxy);

		if (coin->mining->enable) {
			coins.push_back(coin);
			coin->mining->dirs = {};
			for (auto &directory : paths_dir) {
				coin->mining->dirs.push_back(std::make_shared<t_directory_info>(directory, false, std::vector<t_files>()));
			}

			if (settings.HasMember(L"Priority") && (settings[L"Priority"].IsUint())) {
				coin->mining->priority = settings[L"Priority"].GetUint();
			}
			Log(L"Priority %zu", coin->mining->priority);
		}

		if (coin->network->enable_proxy) {
			if (settings.HasMember(L"ProxyPort"))
			{
				if (settings[L"ProxyPort"].IsString())	coin->network->proxyport = toStr(settings[L"ProxyPort"].GetString());
				else if (settings[L"ProxyPort"].IsUint())	coin->network->proxyport = std::to_string(settings[L"ProxyPort"].GetUint());
			}
			Log(L"ProxyPort: %S", coin->network->proxyport.c_str());

			if (settings.HasMember(L"ProxyUpdateInterval") && (settings[L"ProxyUpdateInterval"].IsUint())) {
				coin->network->proxy_update_interval = (size_t)settings[L"ProxyUpdateInterval"].GetUint();
			}
			Log(L"ProxyUpdateInterval: %zu", coin->network->proxy_update_interval);

		}

		if (coin->mining->enable || coin->network->enable_proxy) {

			if (settings.HasMember(L"Mode") && settings[L"Mode"].IsString())
			{
				if (wcscmp(settings[L"Mode"].GetString(), L"solo") == 0) coin->mining->miner_mode = 0;
				else coin->mining->miner_mode = 1;
				Log(L"Mode: %zu", coin->mining->miner_mode);
			}

			if (settings.HasMember(L"Server") && settings[L"Server"].IsString())	coin->network->nodeaddr = toStr(settings[L"Server"].GetString());
			Log(L"Server: %S", coin->network->nodeaddr.c_str());

			if (settings.HasMember(L"Port"))
			{
				if (settings[L"Port"].IsString())	coin->network->nodeport = toStr(settings[L"Port"].GetString());
				else if (settings[L"Port"].IsUint())	coin->network->nodeport = std::to_string(settings[L"Port"].GetUint());
				Log(L"Port %S", coin->network->nodeport.c_str());
			}

			if (settings.HasMember(L"RootUrl") && settings[L"RootUrl"].IsString())	coin->network->noderoot = toStr(settings[L"RootUrl"].GetString());
			Log(L"RootUrl: %S", coin->network->noderoot.c_str());

			if (settings.HasMember(L"SubmitTimeout") && (settings[L"SubmitTimeout"].IsUint())) {
				coin->network->submitTimeout = (size_t)settings[L"SubmitTimeout"].GetUint();
			}
			Log(L"SubmitTimeout: %zu", coin->network->submitTimeout);

			if (settings.HasMember(L"UpdaterAddr") && settings[L"UpdaterAddr"].IsString())	coin->network->updateraddr = toStr(settings[L"UpdaterAddr"].GetString());
			Log(L"Updater address: %S", coin->network->updateraddr.c_str());

			if (settings.HasMember(L"UpdaterPort"))
			{
				if (settings[L"UpdaterPort"].IsString())	coin->network->updaterport = toStr(settings[L"UpdaterPort"].GetString());
				else if (settings[L"UpdaterPort"].IsUint())	 coin->network->updaterport = std::to_string(settings[L"UpdaterPort"].GetUint());
			}
			Log(L"Updater port: %S", coin->network->updaterport.c_str());

			if (settings.HasMember(L"UpdaterRootUrl") && settings[L"UpdaterRootUrl"].IsString())	coin->network->updaterroot = toStr(settings[L"UpdaterRootUrl"].GetString());
			Log(L"UpdaterRootUrl: %S", coin->network->updaterroot.c_str());

			if (settings.HasMember(L"SendInterval") && (settings[L"SendInterval"].IsUint())) coin->network->send_interval = (size_t)settings[L"SendInterval"].GetUint();
			Log(L"SendInterval: %zu", coin->network->send_interval);

			if (settings.HasMember(L"UpdateInterval") && (settings[L"UpdateInterval"].IsUint())) coin->network->update_interval = (size_t)settings[L"UpdateInterval"].GetUint();
			Log(L"UpdateInterval: %zu", coin->network->update_interval);

			if (settings.HasMember(L"TargetDeadline") && (settings[L"TargetDeadline"].IsInt64()))	coin->mining->my_target_deadline = settings[L"TargetDeadline"].GetUint64();
			Log(L"TargetDeadline: %llu", coin->mining->my_target_deadline);
			if (coin->mining->my_target_deadline == 0) {
				Log(L"TargetDeadline has to be >0.");
				fprintf(stderr, "TargetDeadline should be greater than 0. Please check your configuration file.");
				system("pause > nul");
				exit(-1);
			}

			// TODO: elsewhere in json-readers I have string-or-uint, why not here? why have there?
			if (settings.HasMember(L"POC2StartBlock") && (settings[L"POC2StartBlock"].IsUint64())) coin->mining->POC2StartBlock = settings[L"POC2StartBlock"].GetUint64();
			Log(L"POC2StartBlock: %llu", coin->mining->POC2StartBlock);

			if (settings.HasMember(L"enableDiskcoinGensigs"))
				if (!settings[L"enableDiskcoinGensigs"].IsBool())
					Log(L"Ignoring 'enableDiskcoinGensigs': not a boolean");
				else
					coin->mining->enableDiskcoinGensigs = settings[L"enableDiskcoinGensigs"].GetBool();

			if (settings.HasMember(L"UseHTTPS"))
				if (!settings[L"UseHTTPS"].IsBool())
					Log(L"Ignoring 'UseHTTPS': not a boolean");
				else
					coin->network->usehttps = settings[L"UseHTTPS"].GetBool();

			// TODO: refactor as 's = read-extras(s nodename)'
			coin->network->sendextraquery = "";
			if (settings.HasMember(L"ExtraQuery"))
				if (!settings[L"ExtraQuery"].IsObject())
					Log(L"Ignoring 'ExtraQuery': not a json-object");
				else {
					auto const& tmp = settings[L"ExtraQuery"].GetObjectW();
					for (auto item = tmp.MemberBegin(); item != tmp.MemberEnd(); ++item) {
						if (!item->value.IsString()) {
							Log(L"Ignoring 'ExtraQuery/%s': not a string value", item->name.GetString());
							continue;
						}
						coin->network->sendextraquery += "&";
						coin->network->sendextraquery += toStr(item->name.GetString());
						coin->network->sendextraquery += "=";
						coin->network->sendextraquery += toStr(item->value.GetString());
						Log(L"ExtraQuery/%s = %s", item->name.GetString(), item->value.GetString());
					}
				}

			coin->network->sendextraheader = "";
			if (settings.HasMember(L"ExtraHeader"))
				if (!settings[L"ExtraHeader"].IsObject())
					Log(L"Ignoring 'ExtraHeader': not a json-object");
				else {
					auto const& tmp = settings[L"ExtraHeader"].GetObjectW();
					for (auto item = tmp.MemberBegin(); item != tmp.MemberEnd(); ++item) {
						if (!item->value.IsString()) {
							Log(L"Ignoring 'ExtraHeader/%s': not a string value", item->name.GetString());
							continue;
						}
						coin->network->sendextraheader += toStr(item->name.GetString());
						coin->network->sendextraheader += ": ";
						coin->network->sendextraheader += toStr(item->value.GetString());
						coin->network->sendextraheader += "\r\n";
						Log(L"ExtraHeader/%s = %s", item->name.GetString(), item->value.GetString());
					}
				}
		}
	}
}

std::vector<char, heap_allocator<char>> load_config_file(wchar_t const *const filename)
{
	FILE * pFile;

	_wfopen_s(&pFile, filename, L"rt");
	std::unique_ptr<FILE, void(*)(FILE*)> guardPFile(pFile, [](FILE* f) { fclose(f); });

	if (pFile == nullptr)
	{
		fwprintf(stderr, L"\nError. config file %s not found\n", filename);
		system("pause > nul");
		exit(-1);
	}

	_fseeki64(pFile, 0, SEEK_END);
	__int64 const size = _ftelli64(pFile);
	_fseeki64(pFile, 0, SEEK_SET);

	// TODO: leaving it on the heap for now to retain zero-memory effect
	// it's actually somewhat helpful due to the data size mismatch induced by "rt" fopen flags
	std::vector<char, heap_allocator<char>> json_(size + 1, theHeap);
	size_t bytesread = fread_s(json_.data(), size, 1, size, pFile);
	json_[bytesread] = 0;
	guardPFile.reset();

	return json_;
}

DocumentUTF16LE load_config_json(std::vector<char, heap_allocator<char>> const& json_)
{
	DocumentUTF16LE document = parseJsonData<kParseCommentsFlag>(json_);
	if (document.HasParseError()) {
		fprintf(stderr, "\nJSON format error (offset %zu) check miner.conf\n%s\n", document.GetErrorOffset(), GetParseError_En(document.GetParseError())); //(offset %s  %s", (unsigned)document.GetErrorOffset(), (char*)document.GetParseError());
		system("pause > nul");
		exit(-1);
	}
	return document;
}

int load_config(DocumentUTF16LE const& document)
{
	if (document.IsObject())
	{	// Document is a JSON value represents the root of DOM. Root can be either an object or array.

		if (document.HasMember(L"Logging") && document[L"Logging"].IsObject())
		{
			//[GH25]note:no effect, log_init not called, loggingInitialized still FALSE; logline manually delayed for now
			//Log(L"### Loading configuration for Logging ###");

			const ValueUTF16LE& logging = document[L"Logging"];

			if (logging.HasMember(L"logAllGetMiningInfos") && (logging[L"logAllGetMiningInfos"].IsBool()))	loggingConfig.logAllGetMiningInfos = logging[L"logAllGetMiningInfos"].GetBool();
			
			if (logging.HasMember(L"UseLog") && (logging[L"UseLog"].IsBool()))	loggingConfig.enableLogging = logging[L"UseLog"].GetBool();
			
			if (logging.HasMember(L"EnableCsv") && (logging[L"EnableCsv"].IsBool()))	loggingConfig.enableCsv = logging[L"EnableCsv"].GetBool();
		}
	}

	Log_init(); //[GH25]note: do not call before 'loggingConfig.enableLogging' is read from the cfg file; but DO call before any Log()

	// TODO: odd to have log_init in load_config, but it's needed before Log() and it's nice, and right now there's no other place
	// also, but we can't skip log_init if !document.IsObject(), so the condition block had to be split into two parts,
	// or else default loggingConfig.enableLogging=true wouldn't have any effetc due to default loggingInitialized=false, geesh
	// TODO: cleanup; move 'delaying' to Log() so it happens behind the scenes; also pick one default state for logging control flags,
	// or, in fact, maybe remove one flag, why do we even have two in the first place? also, split load_config into two function:
	// load-logging-config and load-whateverelse-config; jsondoc is already in-memory so function splitting does not matter except
	// for code readability and POLA: Log_init() here is REALLY not expected.

	if (document.IsObject())
	{
		Log(L"### Loading configuration for Logging ###"); //[GH25]note:a bit out-ouf-seq but that's the first moment we actually can log

		Log(L"logAllGetMiningInfos: %d", loggingConfig.logAllGetMiningInfos);
		Log(L"UseLog: %d", loggingConfig.enableLogging);
		Log(L"EnableCsv: %d", loggingConfig.enableCsv);


		if (document.HasMember(L"GUI") && document[L"GUI"].IsObject())
		{
			Log(L"### Loading configuration for GUI ###");

			const ValueUTF16LE& gui = document[L"GUI"];

			if (gui.HasMember(L"disable") && (gui[L"disable"].IsBool())) guiConfig.disableGui = gui[L"disable"].GetBool();
		}
		Log(L"disable: %d", guiConfig.disableGui);

		if (document.HasMember(L"WinSizeX") && (document[L"WinSizeX"].IsUint())) guiConfig.size_x = (short)document[L"WinSizeX"].GetUint();
		if (document.HasMember(L"WinSizeY") && (document[L"WinSizeY"].IsUint())) guiConfig.size_y = (short)document[L"WinSizeY"].GetUint();
		Log(L"WinSizeX: %hi", guiConfig.size_x);
		Log(L"WinSizeY: %hi", guiConfig.size_y);

		if (document.HasMember(L"LockWindowSize") && (document[L"LockWindowSize"].IsBool())) guiConfig.lockWindowSize = document[L"LockWindowSize"].GetBool();
		Log(L"LockWindowSize: %d", guiConfig.lockWindowSize);



		if (document.HasMember(L"Paths") && document[L"Paths"].IsArray()) {
			const ValueUTF16LE& Paths = document[L"Paths"];			// Using a reference for consecutive access is handy and faster.
			for (SizeType i = 0; i < Paths.Size(); i++)
			{
				paths_dir.push_back(Paths[i].GetString());
				Log(L"Path: %s", paths_dir[i].c_str());
			}
		}


		Log(L"### Loading coins configuration ###");

		allcoins = { };
		std::vector<std::wstring> coinConfigNodes;
		for (auto item = document.MemberBegin(); item != document.MemberEnd(); ++item)
		{
			auto cstrName = item->name.GetString();
			if (wcsstr(cstrName, L"coin:") == cstrName)
				coinConfigNodes.push_back(cstrName);
		}

		std::transform(coinConfigNodes.begin(), coinConfigNodes.end(), std::back_inserter(allcoins), [&](auto&& coinNodeName) {
			std::wstring coinWideName(coinNodeName.begin() + 5, coinNodeName.end()); // +5 to skip "coin:" prefix
			bool isBurst = coinNodeName.find(L"burst") != std::wstring::npos
				|| coinNodeName.find(L"Burst") != std::wstring::npos
				|| coinNodeName.find(L"BURST") != std::wstring::npos;

			auto coin = std::make_shared<t_coin_info>();
			init_mining_info(coin, coinWideName.c_str(), isBurst ? 0 : 1, isBurst ? 502000 : 0);
			init_coinNetwork(coin);
			loadCoinConfig(document, coinNodeName, coin);

			return coin;
		});

		std::sort(coins.begin(), coins.end(), OrderingByMiningPriority());


		Log(L"### Loading general configuration ###");

		if (document.HasMember(L"CacheSize") && (document[L"CacheSize"].IsUint64())) {
			cache_size1 = document[L"CacheSize"].GetUint64();
			readChunkSize = cache_size1; //sensible default
		}
		Log(L"CacheSize: %zu",  cache_size1);

		if (document.HasMember(L"CacheSize2") && (document[L"CacheSize2"].IsUint64())) cache_size2 = document[L"CacheSize2"].GetUint64();
		Log(L"CacheSize2: %zu", cache_size2);

		if (document.HasMember(L"ReadChunkSize") && (document[L"ReadChunkSize"].IsUint64())) readChunkSize = document[L"ReadChunkSize"].GetUint64();
		Log(L"ReadChunkSize: %zu", readChunkSize);

		if (document.HasMember(L"UseHDDWakeUp") && (document[L"UseHDDWakeUp"].IsBool())) use_wakeup = document[L"UseHDDWakeUp"].GetBool();
		Log(L"UseHDDWakeUp: %d", use_wakeup);

		if (document.HasMember(L"ShowCorruptedPlotFiles") && (document[L"ShowCorruptedPlotFiles"].IsBool())) showCorruptedPlotFiles = document[L"ShowCorruptedPlotFiles"].GetBool();
		Log(L"ShowCorruptedPlotFiles: %d", showCorruptedPlotFiles);

		if (document.HasMember(L"IgnoreSuspectedFastBlocks") && (document[L"IgnoreSuspectedFastBlocks"].IsBool()))
			ignoreSuspectedFastBlocks = document[L"IgnoreSuspectedFastBlocks"].GetBool();
		Log(L"IgnoreSuspectedFastBlocks: %d", ignoreSuspectedFastBlocks);

		if (document.HasMember(L"hddWakeUpTimer") && (document[L"hddWakeUpTimer"].IsUint())) hddWakeUpTimer = document[L"hddWakeUpTimer"].GetUint();
		Log(L"hddWakeUpTimer: %u", hddWakeUpTimer);

		if (document.HasMember(L"bfsTOCOffset") && (document[L"bfsTOCOffset"].IsUint())) bfsTOCOffset = document[L"bfsTOCOffset"].GetUint();
		Log(L"bfsTOCOffset: %u", bfsTOCOffset);

		if (document.HasMember(L"Debug") && (document[L"Debug"].IsBool()))	use_debug = document[L"Debug"].GetBool();
		Log(L"Debug: %d", use_debug);

		if (document.HasMember(L"CheckForUpdateIntervalInDays") && (document[L"CheckForUpdateIntervalInDays"].IsDouble())) {
			checkForUpdateInterval = document[L"CheckForUpdateIntervalInDays"].GetDouble();
		}
		else if (document.HasMember(L"CheckForUpdateIntervalInDays") && (document[L"CheckForUpdateIntervalInDays"].IsInt64())) {
			checkForUpdateInterval = (double) document[L"CheckForUpdateIntervalInDays"].GetInt64();
		}

		if (checkForUpdateInterval < 0) {
			checkForUpdateInterval = 0;
		}
		Log(L"CheckForUpdateIntervalInDays: %f", checkForUpdateInterval);
				
		if (document.HasMember(L"UseBoost") && (document[L"UseBoost"].IsBool())) use_boost = document[L"UseBoost"].GetBool();
		Log(L"UseBoost: %d", use_boost);

#ifdef GPU_ON_C
		if (document.HasMember(L"GPU_Platform") && (document[L"GPU_Platform"].IsInt())) gpu_devices.use_gpu_platform = (size_t)document[L"GPU_Platform"].GetUint();
		Log(L"GPU_Platform: %u", gpu_devices.use_gpu_platform);

		if (document.HasMember(L"GPU_Device") && (document[L"GPU_Device"].IsInt())) gpu_devices.use_gpu_device = (size_t)document[L"GPU_Device"].GetUint();
		Log(L"GPU_Device: %u", gpu_devices.use_gpu_device);
#endif	

	}

	Log(L"=== Config loaded ===");

	return 1;
}

bool load_testmode_config(wchar_t const *const filename)
{
	FILE * pFile;

	_wfopen_s(&pFile, filename, L"rt");
	std::unique_ptr<FILE, void(*)(FILE*)> guardPFile(pFile, [](FILE* f) { fclose(f); });

	if (pFile == nullptr)
	{
		Log(L"=== TestMode disabled (file not found) ===");
		return true;
	}

	_fseeki64(pFile, 0, SEEK_END);
	__int64 const size = _ftelli64(pFile);
	_fseeki64(pFile, 0, SEEK_SET);

	// TODO: leaving it on the heap for now to retain zero-memory effect
	// it's actually somewhat helpful due to the data size mismatch induced by "rt" fopen flags
	std::vector<char, heap_allocator<char>> json_(size + 1, theHeap);
	size_t bytesread = fread_s(json_.data(), size, 1, size, pFile);
	json_[bytesread] = 0;
	guardPFile.reset();

	DocumentUTF16LE document = parseJsonData<kParseCommentsFlag>(json_);
	if (document.HasParseError()) {
		fprintf(stderr, "\nJSON format error (offset %zu) check testmode.conf\n%s\n", document.GetErrorOffset(), GetParseError_En(document.GetParseError())); //(offset %s  %s", (unsigned)document.GetErrorOffset(), (char*)document.GetParseError());
		system("pause > nul");
		exit(-1);
	}

	if (document.IsObject())
	{	// Document is a JSON value represents the root of DOM. Root can be either an object or array.

		if (document.HasMember(L"enabled") && document[L"enabled"].IsBool())
			testmodeConfig.isEnabled = document[L"enabled"].GetBool();

		if (!testmodeConfig.isEnabled)
		{
			Log(L"=== TestMode disabled (disabled by config) ===");
			return true;
		}

		if (document.HasMember(L"RoundReplay") && document[L"RoundReplay"].IsObject())
		{
			Log(L"### Loading configuration for RoundReplay ###");

			const ValueUTF16LE& cfg_replay = document[L"RoundReplay"];

			if (cfg_replay.HasMember(L"coin") && cfg_replay[L"coin"].IsString())
			{
				testmodeConfig.roundReplay.coinName = cfg_replay[L"coin"].GetString();
			}
			else
			{
				Log(L"ERROR: missing or invalid RoundReplay.coin");
				return false;
			}
			Log(L"coin: %s", testmodeConfig.roundReplay.coinName.c_str());

			if (cfg_replay.HasMember(L"rounds") && cfg_replay[L"rounds"].IsArray())
			{
				Log(L"...loading RoundReplay.rounds");

				const ValueUTF16LE& cfg_rounds = cfg_replay[L"rounds"];
				for (SizeType idx_round = 0; idx_round < cfg_rounds.Size(); ++idx_round)
				{
					const ValueUTF16LE& cfg_round = cfg_rounds[idx_round];
					if (!cfg_round.IsObject())
					{
						Log(L"ERROR: invalid RoundReplay.rounds[%zu]", idx_round);
						return false;
					}

					Log(L"...loading RoundReplay.rounds[%zu]", idx_round);
					t_roundreplay_round round;
					{
						if (cfg_round.HasMember(L"height") && cfg_round[L"height"].IsUint64())
							round.height = cfg_round[L"height"].GetUint64();
						else
						{
							Log(L"ERROR: missing or invalid RoundReplay.rounds[%zu].height", idx_round);
							return false;
						}
						Log(L"height: %zu", round.height);

						if (cfg_round.HasMember(L"gensig") && cfg_round[L"gensig"].IsString())
							// TODO: add length assertion and/or parseability assertion and/or parse immediately
							round.signature = cfg_round[L"gensig"].GetString();
						else
						{
							Log(L"ERROR: missing or invalid RoundReplay.rounds[%zu].gensig", idx_round);
							return false;
						}
						Log(L"gensig: %s", round.signature.c_str());

						if (cfg_round.HasMember(L"baseTgt") && cfg_round[L"baseTgt"].IsUint64())
							round.baseTarget = cfg_round[L"baseTgt"].GetUint64();
						else
						{
							Log(L"ERROR: missing or invalid RoundReplay.rounds[%zu].baseTgt", idx_round);
							return false;
						}
						Log(L"baseTgt: %zu", round.baseTarget);

						if (cfg_round.HasMember(L"assume_POCx") && cfg_round[L"assume_POCx"].IsBool())
						{
							round.assume_POC2 = cfg_round[L"assume_POCx"].GetBool();
							Log(L"assume_POCx: %d", round.assume_POC2.value());
						}

						if (cfg_round.HasMember(L"tests") && cfg_round[L"tests"].IsArray())
						{
							Log(L"...loading RoundReplay.rounds[%zu].tests", idx_round);

							const ValueUTF16LE& cfg_tests = cfg_round[L"tests"];
							for (SizeType idx_test = 0; idx_test < cfg_tests.Size(); ++idx_test)
							{
								const ValueUTF16LE& cfg_test = cfg_tests[idx_test];
								if (!cfg_test.IsObject())
								{
									Log(L"ERROR: invalid RoundReplay.rounds[%zu].tests[%zu]", idx_round, idx_test);
									return false;
								}

								Log(L"...loading RoundReplay.rounds[%zu].tests[%zu]", idx_round, idx_test);
								t_roundreplay_round_test test;
								{
									bool hasMode = false;
									if (cfg_test.HasMember(L"mode") && cfg_test[L"mode"].IsString())
									{
										std::wstring tmp = cfg_test[L"mode"].GetString();
										if (tmp == L"normal") { hasMode = true; test.mode = t_roundreplay_round_test::RoundTestMode::RMT_NORMAL; }
										if (tmp == L"offline") { hasMode = true; test.mode = t_roundreplay_round_test::RoundTestMode::RMT_OFFLINE; }
									}
									if (hasMode)
										Log(L"mode: %s", test.mode == t_roundreplay_round_test::RoundTestMode::RMT_NORMAL ? L"normal" : L"offline");
									else
									{
										Log(L"ERROR: missing or invalid RoundReplay.rounds[%zu].tests[%zu].mode", idx_round, idx_test);
										return false;
									}

									if (cfg_test.HasMember(L"assume_account") && cfg_test[L"assume_account"].IsUint64())
									{
										test.assume_account = cfg_test[L"assume_account"].GetUint64();
										Log(L"assume_account: %zu", test.assume_account);
									}
									else
									{
										Log(L"ERROR: missing or invalid RoundReplay.rounds[%zu].tests[%zu].assume_account", idx_round, idx_test);
										return false;
									}

									if (cfg_test.HasMember(L"assume_nonce") && cfg_test[L"assume_nonce"].IsUint64())
									{
										test.assume_nonce = cfg_test[L"assume_nonce"].GetUint64();
										Log(L"assume_nonce: %zu", test.assume_nonce);
									}
									else
									{
										Log(L"ERROR: missing or invalid RoundReplay.rounds[%zu].tests[%zu].assume_nonce", idx_round, idx_test);
										return false;
									}

									if (cfg_test.HasMember(L"assume_scoop") && cfg_test[L"assume_scoop"].IsUint())
									{
										test.assume_scoop = cfg_test[L"assume_scoop"].GetUint();
										Log(L"assume_scoop: %u", test.assume_scoop.value());
									}

									if (cfg_test.HasMember(L"assume_scoop_low") && cfg_test[L"assume_scoop_low"].IsString())
									{
										// TODO: add length assertion and/or parseability assertion and/or parse immediately
										test.assume_scoop_low = cfg_test[L"assume_scoop_low"].GetString();
										Log(L"assume_scoop_low: %s", test.assume_scoop_low.value().c_str());
									}

									if (cfg_test.HasMember(L"assume_scoop_high") && cfg_test[L"assume_scoop_high"].IsString())
									{
										// TODO: add length assertion and/or parseability assertion and/or parse immediately
										test.assume_scoop_high = cfg_test[L"assume_scoop_high"].GetString();
										Log(L"assume_scoop_high: %s", test.assume_scoop_high.value().c_str());
									}

									if (cfg_test.HasMember(L"check_scoop") && cfg_test[L"check_scoop"].IsUint())
									{
										test.check_scoop = cfg_test[L"check_scoop"].GetUint();
										Log(L"check_scoop: %u", test.check_scoop.value());
									}

									if (cfg_test.HasMember(L"check_scoop_low") && cfg_test[L"check_scoop_low"].IsString())
									{
										test.check_scoop_low = cfg_test[L"check_scoop_low"].GetString();
										Log(L"check_scoop_low: %s", test.check_scoop_low.value().c_str());
									}

									if (cfg_test.HasMember(L"check_scoop_high") && cfg_test[L"check_scoop_high"].IsString())
									{
										test.check_scoop_high = cfg_test[L"check_scoop_high"].GetString();
										Log(L"check_scoop_high: %s", test.check_scoop_high.value().c_str());
									}

									if (cfg_test.HasMember(L"check_deadline") && cfg_test[L"check_deadline"].IsUint64())
									{
										test.check_deadline = cfg_test[L"check_deadline"].GetUint64();
										Log(L"check_deadline: %zu", test.check_deadline.value());
									}
								}

								Log(L"...loading RoundReplay.rounds[].tests[] - done");
								round.tests.push_back(test);
							}
						}
					}

					Log(L"...loading RoundReplay.rounds[] - done");
					testmodeConfig.roundReplay.rounds.push_back(round);
				}
			}

			Log(L"...loading configuration for RoundReplay - done");
			testmodeConfig.roundReplay.isEnabled = true;
		}
	}

	Log(L"=== TestMode active ===");
	return true;
}


hwinfo GetCPUInfo(void)
{
	hwinfo result;
	result.AES = InstructionSet::AES();
	result.SSE = InstructionSet::SSE();
	result.SSE2 = InstructionSet::SSE2();
	result.SSE3 = InstructionSet::SSE3();
	result.SSE42 = InstructionSet::SSE42();
	result.AVX = InstructionSet::AVX();
	result.AVX2 = InstructionSet::AVX2();
	result.AVX512F = InstructionSet::AVX512F();

	result.avxsupported = false;
#ifndef __AVX__
	// Checking for AVX requires 3 things:
	// 1) CPUID indicates that the OS uses XSAVE and XRSTORE instructions (allowing saving YMM registers on context switch)
	// 2) CPUID indicates support for AVX
	// 3) XGETBV indicates the AVX registers will be saved and restored on context switch
	int cpuInfo[4];
	__cpuid(cpuInfo, 1);

	bool osUsesXSAVE_XRSTORE = cpuInfo[2] & (1 << 27) || false;
	bool cpuAVXSuport = cpuInfo[2] & (1 << 28) || false;

	if (osUsesXSAVE_XRSTORE && cpuAVXSuport)
	{
		// Check if the OS will save the YMM registers
		unsigned long long xcrFeatureMask = _xgetbv(_XCR_XFEATURE_ENABLED_MASK);
		result.avxsupported = (xcrFeatureMask & 0x6) == 0x6;
	}
#endif

	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	result.vendor = InstructionSet::Vendor();
	result.brand = InstructionSet::Brand();
	result.cores = sysinfo.dwNumberOfProcessors;

	ULONGLONG  TotalMemoryInKilobytes = 0;
	if (GetPhysicallyInstalledSystemMemory(&TotalMemoryInKilobytes))
		result.memory = (unsigned long long)TotalMemoryInKilobytes / 1024;

	return result;
}


void GetPass(std::shared_ptr<t_coin_info> coin, wchar_t const *const p_strFolderPath)
{
	Log(L"Reading passphrase.");
	FILE * pFile;
	size_t len_pass;
	// TODO: get rid of ancient MAX_PATH, test it for long paths afterwards
	std::vector<wchar_t> filename(MAX_PATH);
	swprintf_s(filename.data(), filename.size(), L"%s%s%s%s", p_strFolderPath, L"solosecret-", coin->coinname.c_str(), L".txt");

	//  printf_s("\npass from: %s\n",filename);
	_wfopen_s(&pFile, filename.data(), L"rt");
	std::unique_ptr<FILE, void(*)(FILE*)> guardPFile(pFile, [](FILE* f) { fclose(f); });

	if (pFile == nullptr)
	{
		gui->printToConsole(12, false, false, true, false, L"Error: %s%s%s not found. File is needed for solo mining.", L"solosecret-", coin->coinname.c_str(), L".txt");
		system("pause > nul");
		exit(-1);
	}
	filename.clear();

	_fseeki64(pFile, 0, SEEK_END);
	size_t const lSize = _ftelli64(pFile);
	_fseeki64(pFile, 0, SEEK_SET);

	// TODO: leaving it on the heap for now to retain zero-memory effect
	// it's actually somewhat helpful due to the data size mismatch induced by "rt" fopen flags
	std::vector<char, heap_allocator<char>> buffer(lSize + 1, theHeap);

	len_pass = fread(buffer.data(), 1, lSize, pFile);
	fclose(pFile);

	std::vector<char> pass(lSize * 3);

	for (size_t i = 0, j = 0; i<len_pass; i++, j++)
	{
		if ((buffer[i] == '\n') || (buffer[i] == '\r') || (buffer[i] == '\t')) j--; // Skipping characters, redoing buffer[i] < 20
		else
			if (buffer[i] == ' ') pass[j] = '+';
			else
				if (isalnum(buffer[i]) == 0)
				{
					sprintf_s(pass.data() + j, lSize * 3, "%%%x", (unsigned char)buffer[i]);
					j = j + 2;
				}
				else memcpy(&pass[j], &buffer[i], 1);
	}

	coin->network->solopass = pass.data();
}



size_t GetFiles(std::wstring str, std::vector <t_files> *p_files, bool* bfsDetected, bool forActualFileReading)
{
	HANDLE hFile = INVALID_HANDLE_VALUE;
	WIN32_FIND_DATA   FindFileData;
	size_t count = 0;
	std::vector<std::wstring> path;
	size_t first = 0;
	size_t last = 0;

	bool useSmartFileOrdering = str.size() > 0 && str[0] == L'@';
	if (useSmartFileOrdering) str = str.substr(1);

	//parse path info
	do {
		//check for sequential paths and process path by path
		last = str.find(L"+", first);
		if (last == -1) last = str.length();
		std::wstring str2(str.substr(first, last - first));
		//check if path ends with backslash and append if not
		//but dont append backslash if path starts with backslash (BFS)
		if ((str2.rfind(L"\\") < str2.length() - 1 && str2.find(L"\\") != 0)) str2 = str2 + L"\\";  //dont append BFS
		path.push_back(str2); 
		first = last + 1;
	} while (last != str.length());

	//read all path
	for (auto iter = path.begin(); iter != path.end(); ++iter)
	{
		//check if BFS
		if ((*iter).find(L"\\\\.") == 0)
		{
			*bfsDetected = true;

			//load bfstoc
			if (!LoadBFSTOC(*path.begin()))
			{
				continue;
			}
			//iterate files
			bool stop = false;
			for (int i = 0; i < 72; i++) {
				if (stop) break;
				//check status
				switch (bfsTOC.plotFiles[i].status) {
				//no more files in TOC
				case 0:
					stop = true;
					break;
				//finished plotfile
				case 1:
					p_files->push_back({
						false,
						*path.begin(),
						L"FILE_"+std::to_wstring(i),
						0, // TODO?
						(unsigned long long)bfsTOC.plotFiles[i].nonces * 4096 *64,
						bfsTOC.id, bfsTOC.plotFiles[i].startNonce, bfsTOC.plotFiles[i].nonces, bfsTOC.diskspace/64, bfsTOC.plotFiles[i].startPos, true, true
						});
					count++;
					break;
				//plotting in progress
				case 3:
					break;
				//other file status not supported for mining at the moment
				default:
					break;
				}
			}
		}
		else 
		{
			hFile = FindFirstFile((*iter + L"*").c_str(), &FindFileData);
			if (INVALID_HANDLE_VALUE != hFile)
			{
				do
				{
					if (FILE_ATTRIBUTE_DIRECTORY & FindFileData.dwFileAttributes) continue; //Skip directories
					wchar_t* ekey = wcsstr(FindFileData.cFileName, L"_");
					if (ekey != nullptr)
					{
						wchar_t* estart = wcsstr(ekey + 1, L"_");
						if (estart != nullptr)
						{
							wchar_t* enonces = wcsstr(estart + 1, L"_");
							if (enonces != nullptr)
							{
								unsigned long long key, nonce, nonces, stagger;
								if (swscanf_s(FindFileData.cFileName, L"%llu_%llu_%llu_%llu", &key, &nonce, &nonces, &stagger) == 4)
								{
									long long volpos = 0;
									if (forActualFileReading && useSmartFileOrdering)
									{
										std::wstring wide = *iter + FindFileData.cFileName;
										std::wstring drive = wide.size() > 2 && wide[1] == L':' ? wide.substr(0, 2) : L"";
										if (drive.empty() || 0 != determineNtfsFilePosition(volpos, drive, wide))
											volpos = 0;
									}

									bool p2 = false;
									p_files->push_back({
										false,
										*iter,
										FindFileData.cFileName,
										volpos,
										(((static_cast<ULONGLONG>(FindFileData.nFileSizeHigh) << (sizeof(FindFileData.nFileSizeLow) * 8)) | FindFileData.nFileSizeLow)),
										key, nonce, nonces, stagger, 0, p2, false
										});
									count++;
									continue;
								}

							}
							//POC2 FILE
							unsigned long long key, nonce, nonces;
							if (swscanf_s(FindFileData.cFileName, L"%llu_%llu_%llu", &key, &nonce, &nonces) == 3)
							{
								long long volpos = 0;
								if (forActualFileReading && useSmartFileOrdering)
								{
									std::wstring wide = *iter + FindFileData.cFileName;
									std::wstring drive = wide.size() > 2 && wide[1] == L':' ? wide.substr(0, 2) : L"";
									if (drive.empty() || 0 != determineNtfsFilePosition(volpos, drive, wide))
										volpos = 0;
								}

								bool p2 = true;
								p_files->push_back({
									false,
									*iter,
									FindFileData.cFileName,
									volpos,
									(((static_cast<ULONGLONG>(FindFileData.nFileSizeHigh) << (sizeof(FindFileData.nFileSizeLow) * 8)) | FindFileData.nFileSizeLow)),
									key, nonce, nonces, nonces, 0, p2, false
									});
								count++;
							}

						}
					}
				} while (FindNextFile(hFile, &FindFileData));
				FindClose(hFile);
			}
		}
	}

	if(forActualFileReading && useSmartFileOrdering)
		std::sort(p_files->begin(), p_files->end(), OrderingByPositionOnSequentialMedia());

	return count;
}

unsigned int calcScoop(std::shared_ptr<t_coin_info> coin) {
	Log(L"Calculating scoop for %s", coin->coinname.c_str());
	
	unsigned char scoopgen[40];
	memmove(scoopgen, coin->mining->signature.data(), 32); // TODO: why not CURRENT SIGNATURE?!
	// re ^ it doesnt make MUCH difference, as calcScoop is called from within updateCurrentMiningInfo_withAllScoopWorkersStopped
	// that currentSignature was just refreshed with signature, however there is a slight chance that network thread updated
	// the signature once again and that currentHeight is not matched with signature anymore

	// careful: endianess assumption
	const char* mov = (char*)&coin->mining->currentHeight;
	scoopgen[32] = mov[7];
	scoopgen[33] = mov[6];
	scoopgen[34] = mov[5];
	scoopgen[35] = mov[4];
	scoopgen[36] = mov[3];
	scoopgen[37] = mov[2];
	scoopgen[38] = mov[1];
	scoopgen[39] = mov[0];

	sph_shabal256(&local_32, scoopgen, 40);
	unsigned char xcache[32];
	sph_shabal256_close(&local_32, xcache);

	// careful: endianess assumption
	return (xcache[31] + 256 * xcache[30]) % 4096;
}

// '_withAllScoopWorkersStopped' means it's safe to work on data shared
// with scoop worker threads because there's NO worker threads at this moment
void updateCurrentMiningInfo_withAllScoopWorkersStopped(std::shared_ptr<t_coin_info> coin) {
	const wchar_t* coinName = coin->coinname.c_str();
	Log(L"Updating mining information for %s.", coinName);

	// TODO: why do we need locks inside getsig/etc when all network threads are halted?
	coin->mining->currentSignature = getSignature(coin);

	updateCurrentStrSignature(coin);
	coin->mining->currentHeight = coin->mining->height;
	coin->mining->currentBaseTarget = coin->mining->baseTarget;

	if (coin->mining->enable)
		if (!(testmodeConfig.isEnabled && coin->testround2->assume_scoop.has_value()))
			coin->mining->scoop = calcScoop(coin);
		else
			coin->mining->scoop = coin->testround2->assume_scoop.value();

	if (testmodeConfig.isEnabled)
		if (coin->testround2->check_scoop.has_value())
		{
			auto testresult = coin->mining->scoop == coin->testround2->check_scoop.value();
			coin->testround2->passed_scoop = testresult && coin->testround2->passed_scoop.value_or(true);

			if (!testresult)
				Log(L"TESTMODE: CHECK ERROR: Scoop number differs: %u, expected: %u, height: %llu, gensig: %s",
					coin->mining->scoop, coin->testround2->check_scoop.value(),
					coin->mining->currentHeight, HexString::wstring(coin->mining->currentSignature).c_str());
		}
}

void insertIntoQueue(std::vector<std::shared_ptr<t_coin_info>>& currentQueue, std::shared_ptr<t_coin_info> newCoin,
	std::shared_ptr<t_coin_info > coinCurrentlyMining) {
	bool inserted = false;
	for (auto it = currentQueue.begin(); it != currentQueue.end(); ++it) {
		if (newCoin->mining->priority < (*it)->mining->priority) {
			Log(L"Adding %s to the queue before %s.", newCoin->coinname.c_str(), (*it)->coinname.c_str());
			currentQueue.insert(it, newCoin);
			inserted = true;
			break;
		}
		if (newCoin == (*it)) {
			Log(L"Coin %s already in queue. No action needed", newCoin->coinname.c_str());
			inserted = true;
			if (coinCurrentlyMining && coinCurrentlyMining->mining->state == MiningState::MINING) {
				gui->printBlockEnqueued(newCoin->mining->height, newCoin->coinname, false, false);
			}
			break;
		}
	}
	if (!inserted) {
		Log(L"Adding %s to the end of the queue.", newCoin->coinname.c_str());
		if (coinCurrentlyMining && coinCurrentlyMining->mining->state == MiningState::MINING &&
			newCoin != coinCurrentlyMining &&
			newCoin->mining->priority >= coinCurrentlyMining->mining->priority) {
			gui->printBlockEnqueued(newCoin->mining->height, newCoin->coinname, true, false);
		}
		currentQueue.push_back(newCoin);
	}
	else {
		Log(L"insertIntoQueue: did nothing.");
	}
}


// '_withAllScoopWorkersStopped' means it's safe to work on data shared
// with scoop worker threads because there's NO worker threads at this moment
// TODO: we're shutting down here the network threads as well, maybe other locks can be removed as well?
void newRound_withAllScoopWorkersStopped(std::shared_ptr<t_coin_info> coinCurrentlyMining)
{
	const wchar_t* coinName = coinCurrentlyMining->coinname.c_str();
	Log(L"New round for %s.", coinName);

	// make sure networking threads are inactive, so it's safe to clean curl handles up
	Log(L"Interrupting sender+confirmer threads for %s.", coinCurrentlyMining->coinname.c_str());
	coinCurrentlyMining->locks->stopRoundSpecificNetworkingThreads = true;

	Log(L"Waiting for sender+confirmer threads for %s to shut down.", coinCurrentlyMining->coinname.c_str());
	if (coinCurrentlyMining->network->sender.joinable())
		coinCurrentlyMining->network->sender.join();
	if (coinCurrentlyMining->network->confirmer.joinable())
		coinCurrentlyMining->network->confirmer.join();

	Log(L"Both sender+confirmer threads for %s shut down.", coinCurrentlyMining->coinname.c_str());
	coinCurrentlyMining->locks->stopRoundSpecificNetworkingThreads = false; // actually it's pretty important to do it HERE

	{
		std::lock_guard<std::mutex> lockGuard(coinCurrentlyMining->locks->sessionsLock);
		for (auto it = coinCurrentlyMining->network->sessions.begin(); it != coinCurrentlyMining->network->sessions.end(); ++it) {
			closesocket((*it)->Socket);
		}
		coinCurrentlyMining->network->sessions.clear();
	}
	{
		std::lock_guard<std::mutex> lockGuard(coinCurrentlyMining->locks->sessions2Lock);
		for (auto it = coinCurrentlyMining->network->sessions2.begin(); it != coinCurrentlyMining->network->sessions2.end(); ++it) {
			// it's safe here as long as we ensure that not only WORKER threads are inactive, but coins' SENDER and CONFIRMER threads as well
			curl_easy_cleanup((*it)->curl);
		}
		coinCurrentlyMining->network->sessions2.clear();
	}

	{
		std::lock_guard<std::mutex> lockGuard(coinCurrentlyMining->locks->sharesLock);
		coinCurrentlyMining->mining->shares.clear();
	}

	{
		std::lock_guard<std::mutex> lockGuard(coinCurrentlyMining->locks->bestsLock);
		coinCurrentlyMining->mining->bests.clear();
	}

	// '_withAllScoopWorkersStopped' means it's safe to work on data shared
	// with scoop worker threads because there's NO worker threads at this moment
	updateCurrentMiningInfo_withAllScoopWorkersStopped(coinCurrentlyMining);
	coinCurrentlyMining->mining->deadline = 0;

	if (coinCurrentlyMining->mining->enable && coinCurrentlyMining->mining->state == MiningState::QUEUED) {
		resetDirs(coinCurrentlyMining);
	}

	// if it was inactive, or if we just shut it down to clean up connections - restart it for the new round
	if (!coinCurrentlyMining->network->sender.joinable() && !testmodeConfig.isEnabled) {
		coinCurrentlyMining->network->sender = std::thread(send_i, coinCurrentlyMining);
	}

	// if it was inactive, or if we just shut it down to clean up connections - restart it for the new round
	if (!coinCurrentlyMining->network->confirmer.joinable() && !testmodeConfig.isEnabled) {
		coinCurrentlyMining->network->confirmer = std::thread(confirm_i, coinCurrentlyMining);
	}
}

void handleProxyOnly(std::shared_ptr<t_coin_info> coin) {
	Log(L"Starting proxy only handler for %s.", coin->coinname.c_str());
	while (!exit_flag) {
		if (signaturesDiffer(coin)) {
			Log(L"Signature for %s changed.", coin->coinname.c_str());
			Log(L"Won't add %s to the queue. Proxy only.", coin->coinname.c_str());
			updateOldSignature(coin);
			gui->printBlockEnqueued(coin->mining->height, coin->coinname, false, true);
			
			// TODO: 4398046511104, 240, etc - that are COIN PARAMETERS, these should not be HARDCODED
			if (coin->mining->currentBaseTarget != 0) {
				std::thread{ Csv_Submitted,  coin, coin->mining->currentHeight,
					coin->mining->currentBaseTarget, 4398046511104 / 240 / coin->mining->currentBaseTarget,
					0, true, coin->mining->deadline }.detach();
			}
			newRound_withAllScoopWorkersStopped(coin);
		}
		else {
			std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
}

bool getNewMiningInfo(const std::vector<std::shared_ptr<t_coin_info>>& allCoins,
	std::shared_ptr<t_coin_info > coinCurrentlyMining,
	std::vector<std::shared_ptr<t_coin_info>>& currentQueue) {
	bool newInfoAvailable = false;
	for (auto& pt : allCoins) {
		if (signaturesDiffer(pt)) {
			Log(L"Signature for %s changed.", pt->coinname.c_str());
			updateOldSignature(pt);
			if (pt->mining->enable) {
				// Setting interrupted to false in case the coin with changed signature has been
				// scheduled for continuing.
				pt->mining->state = MiningState::QUEUED;
				Log(L"Inserting %s into queue.", pt->coinname.c_str());
				insertIntoQueue(currentQueue, pt, coinCurrentlyMining);
				newInfoAvailable = true;
			}
		}
	}
	return newInfoAvailable;
}

bool needToInterruptMining(const std::vector<std::shared_ptr<t_coin_info>>& allCoins,
	std::shared_ptr<t_coin_info > coinCurrentlyMining,
	std::vector<std::shared_ptr<t_coin_info>>& currentQueue) {
	if (getNewMiningInfo(allCoins, coinCurrentlyMining, currentQueue)) {
		if (currentQueue.empty()) {
			Log(L"CRITICAL ERROR: Current queue is empty. This should not happen.");
			return false;
		}
		// Checking only the first element, since it has already the highest priority (but lowest value).
		if (currentQueue.front()->mining->enable) {
			if (currentQueue.front()->mining->priority < coinCurrentlyMining->mining->priority) {
				if (coinCurrentlyMining->mining->state == MiningState::MINING) {
					Log(L"Interrupting current mining progress. %s has a higher priority than %s.",
						currentQueue.front()->coinname.c_str(), coinCurrentlyMining->coinname.c_str());
				}
				return true;
			}
			else {
				for (auto& pt : currentQueue) {
					if (pt == coinCurrentlyMining) {
						Log(L"Interrupting current mining progress. New %s block.", coinCurrentlyMining->coinname.c_str());
						return true;
					}
				}
			}
		}
	}
	return false;
}

unsigned long long getPlotFilesSize(std::vector<std::wstring>& directories, bool log, std::vector<t_files>& all_files, bool& bfsDetected) {
	unsigned long long size = 0;
	for (auto iter = directories.begin(); iter != directories.end(); ++iter) {
		std::vector<t_files> files;
		GetFiles(*iter, &files, &bfsDetected, false);

		unsigned long long tot_size = 0;
		for (auto it = files.begin(); it != files.end(); ++it) {
			tot_size += it->Size;
			all_files.push_back(*it);
		}
		if (log) {
			gui->printPlotsInfo(*iter, files.size(), tot_size);
		}
		size += tot_size;
	}
	return size;
}

unsigned long long getPlotFilesSize(std::vector<std::wstring>& directories, bool log) {
	bool dummyvar;
	std::vector<t_files> dump;
	return getPlotFilesSize(directories, log, dump, dummyvar);
}

unsigned long long getPlotFilesSize(std::vector<std::shared_ptr<t_directory_info>> dirs) {
	unsigned long long size = 0;
	for (auto &dir : dirs) {
		for (auto &f : dir->files) {
			if (!f.done) {
				size += f.Size;
			}
		}
	}
	return size;
}

void closeMiner() {
	if (exitHandled) {
		return;
	}
	Log(L"Closing miner.");
	exitHandled = true;

	exit_flag = true;
	stopThreads = true;
	for (auto& coin : allcoins)
		coin->locks->stopRoundSpecificNetworkingThreads = true;

	Log(L"Waiting for worker threads to shut down.");
	for (auto it = worker.begin(); it != worker.end(); ++it)
	{
		if (it->second.joinable()) {
			it->second.join();
		}
	}
	Log(L"All worker threads shut down.");
	
	for (auto& coin : allcoins)
		if (coin->updaterThread.joinable()) coin->updaterThread.join();
	for (auto& coin : allcoins)
		if (coin->proxyThread.joinable()) coin->proxyThread.join();
	for (auto& coin : allcoins)
		if (coin->proxyOnlyThread.joinable()) coin->proxyOnlyThread.join();

	if (updateChecker.joinable()) updateChecker.join();

	for (auto& coin : allcoins)
		if (coin->network->sender.joinable()) coin->network->sender.join();

	for (auto& coin : allcoins)
	{
		std::lock_guard<std::mutex> lockGuard(coin->locks->sessionsLock);
		for (auto it = coin->network->sessions.begin(); it != coin->network->sessions.end(); ++it) {
			closesocket((*it)->Socket);
		}
		coin->network->sessions.clear();
	}
	for (auto& coin : allcoins)
	{
		std::lock_guard<std::mutex> lockGuard(coin->locks->sessions2Lock);
		for (auto it = coin->network->sessions2.begin(); it != coin->network->sessions2.end(); ++it) {
			curl_easy_cleanup((*it)->curl);
		}
		coin->network->sessions2.clear();
	}

	// TODO: cleanup this whole destructor insanity
	p_minerPath.~vector();

	curl_global_cleanup();
	WSACleanup();
	Log(L"exit");
	Log_end();
	gui.reset();

	worker.~map();
	worker_progress.~map();
	paths_dir.~vector();

	for (auto& coin : allcoins)
	{
		coin->mining->bests.~vector();
		coin->mining->shares.~vector();
		coin->network->sessions.~vector();
		coin->network->sessions2.~vector();
	}
}

BOOL WINAPI OnConsoleClose(DWORD dwCtrlType)
{
	if (dwCtrlType == CTRL_CLOSE_EVENT) {
		closeMiner();
		return TRUE;
	}
	return FALSE;
}


void initMiningOrProxy(std::shared_ptr<t_coin_info> coin)
{
	if (coin->mining->enable || coin->network->enable_proxy) {

		coin->locks->stopRoundSpecificNetworkingThreads = false;

		coin->mining->shares.reserve(20);
		coin->mining->bests.reserve(4);
		coin->network->sessions.reserve(20);
		coin->network->sessions2.reserve(20);

		// TODO:below: why do we even need this `hostname_to_ip` proc?
		// it is written to crash the process if something goes wrong
		// but it doesn't DO anything except logging the result
		// also, much later the network may come and go, and the code copes with it
		// this is some diag or fast-fail code that prevents simple CFG issues,
		// to quit the miner ASAP when you forgot to check the network,
		// but.. it shouldn't be HERE and shouldn't be MANDATORY
		// I WANT to start my miner even if my f* ISP went offline,
		// and the miner should simply resume when the network comes back
		{
			std::vector<char> nodeip(50); // actually, for IPv4 15 is enough, and 45 for IPv6,
			std::vector<char> updaterip(50); // so 50 here is overallocated for no real gain

			hostname_to_ip(coin->network->nodeaddr.c_str(), nodeip.data());
			gui->printNetworkHostResolution(L"pool address", coin->coinname.c_str(),
				coin->network->nodeaddr, nodeip, coin->network->nodeport, coin->network->noderoot);

			// TODO: why this special condition is here? why there is none for nodeaddr?
			if (coin->network->updateraddr.length() > 3)
				hostname_to_ip(coin->network->updateraddr.c_str(), updaterip.data());
			gui->printNetworkHostResolution(L"updater address", coin->coinname.c_str(),
				coin->network->updateraddr, updaterip, coin->network->updaterport, coin->network->updaterroot);
		}

		// TODO: vectorize/etc
		// TODO: RtlSecureZeroMemory, everywhere, seriously?
		// coin->mining->oldSignature.fill(0);
		// coin->mining->signature.fill(0);
		// coin->mining->currentSignature.fill(0);
		// below: not needed, those are recently-constructed anyways
		// coin->mining->str_signature.clear();
		// coin->mining->current_str_signature.clear();
	}
}

std::shared_ptr<t_coin_info> cloneCoinSetup(std::shared_ptr<t_coin_info> const & src)
{
	auto result = std::make_shared<t_coin_info>();

	result->coinname = src->coinname;
	//result->logging - nah, in testmode it is disabled anyways
	init_mining_info(result, src->coinname, src->mining->priority, src->mining->POC2StartBlock);
	std::transform(src->mining->dirs.begin(), src->mining->dirs.end(), std::back_inserter(result->mining->dirs), [](auto&& item) { return std::make_shared<t_directory_info>(*item); });
	result->network = src->network;
	initMiningOrProxy(result);
	return result;
}

int wmain(int argc, wchar_t **argv) {
	//init
	SetConsoleCtrlHandler(OnConsoleClose, TRUE);
	atexit(closeMiner);

	hHeap = GetProcessHeap();
	HeapSetInformation(NULL, HeapEnableTerminationOnCorruption, NULL, 0);
	theHeap = { hHeap, HEAP_ZERO_MEMORY };

	//initialize time 
	LARGE_INTEGER li;
	__int64 start_threads_time, end_threads_time, curr_time;
	QueryPerformanceFrequency(&li);
	double pcFreq = double(li.QuadPart);

	std::vector<std::thread> generator;

	unsigned long long bytesRead = 0;

	// TODO: make it std::(w)str, use sstream, use better path ops
	// TODO: get rid of ancient MAX_PATH, test it for long paths afterwards
	//get application path
	size_t const cwdsz = GetCurrentDirectory(0, 0);
	p_minerPath.resize(cwdsz + 2);
	GetCurrentDirectory(DWORD(cwdsz), p_minerPath.data());
	wcscat_s(p_minerPath.data(), cwdsz + 2, L"\\");


	// init 3rd party libs
	curl_global_init(CURL_GLOBAL_DEFAULT);

	// Initialize configuration defaults
	init_logging_config();
	init_gui_config();

	// TODO: below: cut that [1][2] argv crap and refactor it to proper position-agnostic param parsing

	//load config
	{
		// TODO: make it std::(w)str, use sstream, use better path ops
		// TODO: get rid of ancient MAX_PATH, test it for long paths afterwards
		std::vector<wchar_t> conf_filename(MAX_PATH);

		//config-file: check -config flag or default to miner.conf
		if ((argc >= 4) && (wcscmp(argv[3], L"-config") == 0)) {
			if (wcsstr(argv[4], L":\\")) swprintf_s(conf_filename.data(), conf_filename.size(), L"%s", argv[4]);
			else swprintf_s(conf_filename.data(), conf_filename.size(), L"%s%s", p_minerPath.data(), argv[4]);
		}
		else
		if ((argc >= 2) && (wcscmp(argv[1], L"-config") == 0)) {
			if (wcsstr(argv[2], L":\\")) swprintf_s(conf_filename.data(), conf_filename.size(), L"%s", argv[2]);
			else swprintf_s(conf_filename.data(), conf_filename.size(), L"%s%s", p_minerPath.data(), argv[2]);
		}
		else swprintf_s(conf_filename.data(), conf_filename.size(), L"%s%s", p_minerPath.data(), L"miner.conf");

		auto buff = load_config_file(conf_filename.data());
		auto doc = load_config_json(buff);
		load_config(doc);
	}

	// load testmode config
	{
		// TODO: make it std::(w)str, use sstream, use better path ops
		// TODO: get rid of ancient MAX_PATH, test it for long paths afterwards
		std::vector<wchar_t> conf_filename(MAX_PATH);

		//config-file: check -config flag or default to miner.conf
		if ((argc >= 4) && (wcscmp(argv[3], L"-testconfig") == 0)) {
			if (wcsstr(argv[4], L":\\")) swprintf_s(conf_filename.data(), conf_filename.size(), L"%s", argv[4]);
			else swprintf_s(conf_filename.data(), conf_filename.size(), L"%s%s", p_minerPath.data(), argv[4]);
		}
		else
		if ((argc >= 2) && (wcscmp(argv[1], L"-testconfig") == 0)) {
			if (wcsstr(argv[2], L":\\")) swprintf_s(conf_filename.data(), conf_filename.size(), L"%s", argv[2]);
			else swprintf_s(conf_filename.data(), conf_filename.size(), L"%s%s", p_minerPath.data(), argv[2]);
		}
		else swprintf_s(conf_filename.data(), conf_filename.size(), L"%s%s", p_minerPath.data(), L"testmode.conf");

		if (!load_testmode_config(conf_filename.data()))
		{
			fwprintf(stderr, L"\nError. testmode config file %s is broken\n", conf_filename.data());
			system("pause > nul");
			exit(-1);
		}
	}

	if (testmodeConfig.isEnabled)
		loggingConfig.enableCsv = false;

	std::vector<std::shared_ptr<t_coin_info>> proxycoins;
	std::copy_if(allcoins.begin(), allcoins.end(), std::back_inserter(proxycoins), [](auto&& it) { return it->network->enable_proxy; });
	std::vector<std::string> proxyports;
	std::transform(proxycoins.begin(), proxycoins.end(), std::back_inserter(proxyports), [](auto&& it) { return it->network->proxyport; });
	size_t beforeUniq = proxyports.size();
	std::sort(proxyports.begin(), proxyports.end());
	proxyports.erase(std::unique(proxyports.begin(), proxyports.end()), proxyports.end());
	size_t afterUniq = proxyports.size();

	if (afterUniq != beforeUniq) {
		fprintf(stderr, "\nError. Ports for multiple proxies should not be the same. Please check your configuration.\n");
		system("pause > nul");
		exit(-1);
	}

	Csv_Init();

	Log(L"Miner path: %s", p_minerPath.data());
	Log(L"Miner process elevation: %s", IsElevated() ? L"active" : L"inactive");

	Gui_init();

	gui->printHeadlineTitle(appname, version, IsElevated());
	gui->printWallOfCredits(history);
	gui->printHWInfo(GetCPUInfo());

	std::vector<std::shared_ptr<t_coin_info>> activecoins;
	std::copy_if(allcoins.begin(), allcoins.end(), std::back_inserter(activecoins), [](auto&& it) { return it->network->enable_proxy || it->mining->enable; });
	if (activecoins.size() == 0) {
		gui->printToConsole(12, false, false, true, false, L"Mining and proxies are disabled for all coins. Please check your configuration.");
		system("pause > nul");
		exit(-1);
	}

	std::vector<std::shared_ptr<t_coin_info>> miningcoins;
	std::copy_if(allcoins.begin(), allcoins.end(), std::back_inserter(miningcoins), [](auto&& it) { return it->mining->enable; });
	for (auto& coin : miningcoins)
		if ((coin->mining->miner_mode == 0))
			GetPass(coin, p_minerPath.data());

	// server address and port
	Log(L"Searching servers...");
	WSADATA wsaData;

	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		gui->printToConsole(-1, false, false, true, false, L"WSAStartup failed");
		system("pause > nul");
		exit(-1);
	}

	for (auto& coin : allcoins)
		initMiningOrProxy(coin);
	
	// File info
	gui->printPlotsStart();
	
	bool bfsDetected = false;
	std::vector<t_files> all_files;
	total_size = getPlotFilesSize(paths_dir, true, all_files, bfsDetected);

	if (bfsDetected && !IsElevated()) {
		Log(L"BFS path detected and elevation is missing, attempting to elevate");
		gui->printToConsole(12, false, true, true, false, L"BFS path detected and elevation is missing, attempting to elevate.");
		if (RestartWithElevation(argc, argv)) {
			Log(L"Elevation succeeded. New process will takeover. Exiting.");
			gui->printToConsole(12, false, true, true, false, L"Elevation succeeded. New process will takeover. Exiting.");
			exit(0);
		}

		Log(L"Elevation failed. BFS plots cannot be accessed and will be ignored.");
		gui->printToConsole(12, false, true, true, false, L"Elevation failed. BFS plots cannot be accessed and will be ignored.");
	}

	gui->printPlotsEnd(total_size);
	
	if (total_size == 0 && miningcoins.size() > 0) {
		gui->printToConsole(12, false, true, true, false,
			L"Plot files not found...please check the \"PATHS\" parameter in your config file.");
		system("pause > nul");
		exit(-1);
	}
	else if (total_size == 0) {
		gui->printToConsole(12, false, true, true, false, L"\nNo plot files found.");
	}

	// Check overlapped plots
	for (size_t cx = 0; cx < all_files.size(); cx++) {
		for (size_t cy = cx + 1; cy < all_files.size(); cy++) {
			if (all_files[cy].Key == all_files[cx].Key)
				if (all_files[cy].StartNonce >= all_files[cx].StartNonce) {
					if (all_files[cy].StartNonce < all_files[cx].StartNonce + all_files[cx].Nonces) {
						gui->printToConsole(12, false, true, true, false, L"WARNING: %s%s and \n%s%s are overlapped",
							all_files[cx].Path.c_str(), all_files[cx].Name.c_str(), all_files[cy].Path.c_str(), all_files[cy].Name.c_str());
					}
				}
				else
					if (all_files[cy].StartNonce + all_files[cy].Nonces > all_files[cx].StartNonce) {
						gui->printToConsole(12, false, true, true, false, L"WARNING: %s%s and \n%s%s are overlapped",
							all_files[cx].Path.c_str(), all_files[cx].Name.c_str(), all_files[cy].Path.c_str(), all_files[cy].Name.c_str());
					}
		}
	}
	//all_files.~vector();   // ???

	for(auto& coin : allcoins)
		if (coin->network->submitTimeout < 1000) {
			gui->printToConsole(8, false, true, false, true, L"Timeout for %s deadline submissions is set to %u ms, which is a low value.", coin->coinname.c_str(), coin->network->submitTimeout);
		}

	proxyOnly = miningcoins.size() == 0 && proxycoins.size() > 0;
	if (proxyOnly) {
		Log(L"Running as proxy only.");
		gui->printToConsole(8, false, true, false, true, L"Running as proxy only.");
	}

	// Run Proxy
	for (auto& coin : allcoins)
		// note: theoretically, PROXY mode could work with ROUNDREPLAY as well to test slaves.. but scoop data couldn't be checked
		if (coin->network->enable_proxy && !testmodeConfig.isEnabled)
		{
			coin->proxyThread = std::thread(proxy_i, coin);
			gui->printThreadActivity(coin->coinname, L"proxy", L"started");
		}

	// Run version checker
	if (checkForUpdateInterval > 0 && !testmodeConfig.isEnabled) {
		updateChecker = std::thread(checkForUpdate);
	}

	// Run updater
	for (auto& coin : allcoins)
		if ((coin->mining->enable || coin->network->enable_proxy) && !testmodeConfig.isEnabled)
		{
			coin->updaterThread = std::thread(updater_i, coin);
			gui->printThreadActivity(coin->coinname, L"updater", L"started");
		}

	std::vector<std::shared_ptr<t_coin_info>> queue;

	// Run proxy-only
	for (auto& coin : allcoins)
		// note: theoretically, PROXY mode could work with ROUNDREPLAY as well to test slaves.. but scoop data couldn't be checked
		if (!coin->mining->enable && coin->network->enable_proxy && !testmodeConfig.isEnabled)
		{
			coin->proxyOnlyThread = std::thread(handleProxyOnly, coin);
			gui->printThreadActivity(coin->coinname, L"proxy-only", L"started");
		}

	if (proxyOnly) {
		while (!exit_flag)
		{
			
			switch (gui->bm_wgetchMain())
			{
			case 'q':
				exit_flag = true;
				break;
			}
			
			std::wostringstream connQual;
			bool pastfirst = false;
			for (auto& coin : activecoins)
			{
				if (pastfirst) connQual << L'|';
				connQual << std::setw(3) << getNetworkQuality(coin) << L'%';
				pastfirst = true;
			}
			gui->printConnQuality(activecoins.size(), connQual.str());

			std::this_thread::yield();
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
	}
	else {
		// Main loop
		// Create Shabal Contexts
#ifdef __AVX512F__
		simd512_mshabal_init(&global_512, 256);
		//create minimal context
		global_512_fast.out_size = global_512.out_size;
		for (int j = 0; j < 704; j++)
			global_512_fast.state[j] = global_512.state[j];
		global_512_fast.Whigh = global_512.Whigh;
		global_512_fast.Wlow = global_512.Wlow;
#else
#ifdef __AVX2__
		simd256_mshabal_init(&global_256, 256);
		//create minimal context
		global_256_fast.out_size = global_256.out_size;
		for (int j = 0; j < 352; j++)
			global_256_fast.state[j] = global_256.state[j];
		global_256_fast.Whigh = global_256.Whigh;
		global_256_fast.Wlow = global_256.Wlow;

#else
#ifdef __AVX__
		simd128_mshabal_init(&global_128, 256);
		//create minimal context
		global_128_fast.out_size = global_128.out_size;
		for (int j = 0; j < 176; j++)
			global_128_fast.state[j] = global_128.state[j];
		global_128_fast.Whigh = global_128.Whigh;
		global_128_fast.Wlow = global_128.Wlow;

#else
	//disable for non SSE!
		simd128_mshabal_init(&global_128, 256);
		//create minimal context
		global_128_fast.out_size = global_128.out_size;
		for (int j = 0; j < 176; j++)
			global_128_fast.state[j] = global_128.state[j];
		global_128_fast.Whigh = global_128.Whigh;
		global_128_fast.Wlow = global_128.Wlow;

#endif
#endif 
#endif 
		//context for signature calculation
		sph_shabal256_init(&global_32);
		memcpy(&local_32, &global_32, sizeof(global_32));

		// first-time status update for any coin (whichever gets its first GMI first)
		{
			if (testmodeConfig.isEnabled) {
				// spoof COINQUEUE with multiple entries for the same coin! I'm pretty sure that only the UPDATER thread reorders entries and the main loop simply POPs!
				auto const referenceSetup = std::find_if(allcoins.begin(), allcoins.end(), [](auto&& coin) { return coin->coinname == testmodeConfig.roundReplay.coinName; });

				if (referenceSetup == allcoins.end())
				{
					gui->printToConsole(12, false, true, true, false, L"TestMode config error: coin %s not found in miner.conf", testmodeConfig.roundReplay.coinName.c_str());
					system("pause > nul");
					exit(-1);
				}

				// each round*test pair is a SEPARATE mining round that the testmode must execute
				for(auto&& round : testmodeConfig.roundReplay.rounds)
					for (auto&& test : round.tests)
					{
						auto newCoin = cloneCoinSetup(*referenceSetup);

						newCoin->mining->height = round.height;
						newCoin->mining->str_signature = round.signature;
						newCoin->mining->signature = *HexString::arrayfrom<32>(round.signature.c_str());
						newCoin->mining->baseTarget = round.baseTarget;

						newCoin->testround1 = &round;
						newCoin->testround2 = &test;

						queue.push_back(newCoin);
					}
			}
		}

		unsigned long long lastBytesRead = 0, lastRound_size = 0;
		double lastThread_time = 0, lastThreads_speed = 0;
		std::shared_ptr<t_coin_info> miningCoin;
		while (!exit_flag)
		{
			worker.clear();
			worker_progress.clear();
			stopThreads = false;
			int oldThreadsRunning = -1;
			double thread_time;

			if(testmodeConfig.isEnabled)
				Log(L"=== TestMode active ===");

			if (!testmodeConfig.isEnabled && queue.empty())
			{
				Log(L"Waiting for new round");

				// Waiting for mining information
				std::wstring prevConnQual;
				bool newDataAvailable = false;
				while (!exit_flag && !newDataAvailable && !testmodeConfig.isEnabled) {
					switch (gui->bm_wgetchMain())
					{
					case 'q':
						exit_flag = true;
						break;
					case 'f':
						resetFileStats();
						break;
					}

					if (exit_flag) {
						Log(L"Exitting miner.");
						break;
					}

					std::wostringstream connQual;
					bool pastfirst = false;
					for (auto& coin : activecoins)
					{
						if (pastfirst) connQual << L'|';
						connQual << std::setw(3) << getNetworkQuality(coin) << L'%';
						pastfirst = true;
					}

					// we're busy-spinning, no UI updates happen
					// print something out or else user won't be notified that network is down
					// but try not spamming too much
					std::wstring currConnQual = connQual.str();
					if (prevConnQual != currConnQual)
					{
						prevConnQual = currConnQual;
						gui->printScanProgress(activecoins.size(), connQual.str(),
							lastBytesRead, lastRound_size,
							lastThread_time, lastThreads_speed,
							0);
					}

					for (auto& c : coins) {
						if (c->mining->enable && getHeight(c) != 0) {
							newDataAvailable = getNewMiningInfo(coins, nullptr, queue);
							break;
						}
					}
					std::this_thread::yield();
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}

			for (auto& coin : allcoins)
				Log(L"%s height: %llu", coin->coinname.c_str(), getHeight(coin));

			std::wstring out = L"Coin queue: ";
			for (auto& c : queue) {
				out += c->coinname + L" (" + std::to_wstring(c->mining->height) + L")";
				if (c != queue.back())	out += L", "; else out += L".";
			}
			Log(out.c_str());

			miningCoin = queue.front();
			setnewMiningInfoReceived(miningCoin, false);
			queue.erase(queue.begin());

			newRound_withAllScoopWorkersStopped(miningCoin);

			// TODO: there doesn't seem to be a way for a 'miningcoin' to be 'not enabled'
			if (miningCoin->mining->enable) {
				auto verb = (miningCoin->mining->state == MiningState::INTERRUPTED) ? L"Continuing" : L"New";
				Log(L"------------------------    %s %s block: %llu", verb, miningCoin->coinname.c_str(), miningCoin->mining->currentHeight);

				// TODO: 4398046511104, 240, etc - that are COIN PARAMETERS, these should not be HARDCODED
				gui->printRoundChangeInfo(miningCoin->mining->state == MiningState::INTERRUPTED,
					miningCoin->mining->currentHeight,
					miningCoin->coinname,
					miningCoin->mining->currentBaseTarget,
					4398046511104 / 240 / miningCoin->mining->currentBaseTarget,
					miningCoin->isPoc2Round());
			}

			QueryPerformanceCounter((LARGE_INTEGER*)&start_threads_time);
			double threads_speed = 0;

			// Run worker threads
			std::vector<std::wstring> roundDirectories;
			// in offline test mode, we dont need parallel workers to read many plot files, it's one specific nonce/scoop generated on the fly
			size_t workersNeeded = testmodeConfig.isEnabled && miningCoin->testround2->mode == t_roundreplay_round_test::RoundTestMode::RMT_OFFLINE ? 1 : miningCoin->mining->dirs.size();
			for (size_t i = 0; i < workersNeeded; i++)
			{
				if (miningCoin->mining->dirs.at(i)->done) {
					// This directory has already been processed. Skipping.
					Log(L"Skipping directory %s", miningCoin->mining->dirs.at(i)->dir.c_str());
					continue;
				}
				worker_progress[i] = { i, 0, true };
				worker[i] = std::thread(work_i, miningCoin, miningCoin->mining->dirs.at(i), i);
				roundDirectories.push_back(miningCoin->mining->dirs.at(i)->dir);
			}

			unsigned long long round_size = 0;

			// TODO: isn't this check swapped?
			// 'roundDirectories' seems to be filtered by 'done' and miningCoin->mining->dirs seems to be all of them?
			// but getPlotFilesSize also filters by f.done
			// then, WHY do we have this IF at all? at first glance, both of them seem to be bound to return identical results
			if (miningCoin->mining->state == MiningState::INTERRUPTED) {
				round_size = getPlotFilesSize(miningCoin->mining->dirs);
			}
			else {
				round_size = getPlotFilesSize(roundDirectories, false);
			}

			miningCoin->mining->state = MiningState::MINING;
			Log(L"Directories in this round: %zu", roundDirectories.size());
			Log(L"Round size: %llu GB", round_size / 1024 / 1024 / 1024);

			// Wait until signature changed or exit
			// or current scanning is done (no point in busy-spinning here when all threads already have processed all their data)
			while (!exit_flag 
				&& miningCoin->mining->state == MiningState::MINING
				&& (!haveReceivedNewMiningInfo(coins) || !needToInterruptMining(coins, miningCoin, queue))
			)
			{
				switch (gui->bm_wgetchMain())
				{
				case 'q':
					exit_flag = true;
					break;
				case 'f':
					resetFileStats();
					break;
				}

				if (exit_flag) {
					Log(L"Exitting miner.");
					break;
				}

				bytesRead = 0;
				int threads_running = 0;
				for (auto it = worker_progress.begin(); it != worker_progress.end(); ++it)
				{
					bytesRead += it->second.Reads_bytes;
					threads_running += it->second.isAlive;
				}
				if (threads_running != oldThreadsRunning) {
					Log(L"threads_running: %i", threads_running);
				}
				oldThreadsRunning = threads_running;

				if (threads_running)
				{
					QueryPerformanceCounter((LARGE_INTEGER*)&end_threads_time);
					threads_speed = (double)(bytesRead / (1024 * 1024)) / ((double)(end_threads_time - start_threads_time) / pcFreq);
					thread_time = (double)(end_threads_time - start_threads_time) / pcFreq;
				}
				else {
					/*
					if (can_generate == 1)
					{
					Log(L"\nStart Generator. ");
					for (size_t i = 0; i < std::thread::hardware_concurrency()-1; i++)	generator.push_back(std::thread(generator_i, i));
					can_generate = 2;
					}
					*/
					//Work Done! Cleanup / Prepare
					if (miningCoin->mining->state == MiningState::MINING) {
						Log(L"Round done.");
						Log(L"Bytes read: %llu", bytesRead);
						miningCoin->mining->state = MiningState::DONE; // NTS: it seems FINE to not synchronize write here, as all writes to STATE seem to occur on WMAIN thread
						//Display total round time
						QueryPerformanceCounter((LARGE_INTEGER*)&end_threads_time);
						thread_time = (double)(end_threads_time - start_threads_time) / pcFreq;

						// TODO: there doesn't seem to be a way for a 'miningcoin' to be 'not enabled'
						if (miningCoin->mining->enable) {
							Log(L"Total round time: %.1f seconds", thread_time);
							if (use_debug)
							{
								gui->debugRoundTime(thread_time);
							}
						}
						//prepare
						memcpy(&local_32, &global_32, sizeof(global_32));
					}
					else if (!queue.empty()) {
						Log(L"Next coin in queue.");
						break;
					}
					else if (testmodeConfig.isEnabled) {
						exit_flag = true;
						continue;
					}

					if (use_wakeup)
					{
						QueryPerformanceCounter((LARGE_INTEGER*)&curr_time);
						if ((curr_time - end_threads_time) / pcFreq > hddWakeUpTimer)
						{
							bool dummyvar;
							std::vector<t_files> tmp_files;
							for (size_t i = 0; i < paths_dir.size(); i++)		GetFiles(paths_dir[i], &tmp_files, &dummyvar, false);
							if (use_debug)
							{
								gui->printToConsole(7, true, false, true, false, L"HDD, WAKE UP !");
							}
							end_threads_time = curr_time;
						}
					}
				}

				std::wostringstream connQual;
				bool pastfirst = false;
				for (auto& coin : activecoins)
				{
					if (pastfirst) connQual << L'|';
					connQual << std::setw(3) << getNetworkQuality(coin) << L'%';
					pastfirst = true;
				}

				// TODO: there doesn't seem to be a way for a 'miningcoin' to be 'not enabled'
				if (miningCoin->mining->enable && round_size > 0) {
					gui->printScanProgress(activecoins.size(), connQual.str(),
						bytesRead, round_size,
						thread_time, threads_speed,
						miningCoin->mining->deadline);

					lastBytesRead = bytesRead;
					lastRound_size = round_size;
					lastThread_time = thread_time;
					lastThreads_speed = threads_speed;
				}
				
				printFileStats();
				
				std::this_thread::yield();
				std::this_thread::sleep_for(std::chrono::milliseconds(50));
			}


			// make sure worker threads are inactive, so these don't waste time mining on old height
			Log(L"Interrupting worker threads.");
			stopThreads = true;   // Tell all threads to stop

			Log(L"Waiting for worker threads to shut down.");
			for (auto it = worker.begin(); it != worker.end(); ++it)
			{
				if (it->second.joinable()) {
					it->second.join();
				}
			}
			Log(L"All worker threads shut down.");


			if (miningCoin->mining->state == MiningState::MINING) {
				// Checking if all directories are done for the rare case that workers finished minMiningState::ing
				// in the same loop iteration as they have been interrupted. In that case mining has been
				// finished.
				if (allDirsDone(miningCoin)) {
					Log(L"Round done.");
					Log(L"Bytes read: %llu", bytesRead);
					miningCoin->mining->state = MiningState::DONE;
					//Display total round time
					QueryPerformanceCounter((LARGE_INTEGER*)&end_threads_time);
					thread_time = (double)(end_threads_time - start_threads_time) / pcFreq;

					if (miningCoin->mining->enable) {
						Log(L"Total round time: %.1f seconds", thread_time);
						if (use_debug)
						{
							gui->debugRoundTime(thread_time);
						}
					}
					//prepare
					memcpy(&local_32, &global_32, sizeof(global_32));
				}
				else {
					miningCoin->mining->state = MiningState::INTERRUPTED;
					Log(L"Mining %s has been interrupted by a coin with higher priority.", miningCoin->coinname.c_str());
					gui->printRoundInterrupt(miningCoin->mining->currentHeight, miningCoin->coinname);
					// Queuing the interrupted coin.
					insertIntoQueue(queue, miningCoin, miningCoin);
				}
			}
			else if (!exit_flag) {
				Log(L"New block, no mining has been interrupted.");
			}

			// TODO: 4398046511104, 240, etc - that are COIN PARAMETERS, these should not be HARDCODED
			if (!testmodeConfig.isEnabled)
				std::thread{ Csv_Submitted,  miningCoin, miningCoin->mining->currentHeight, miningCoin->mining->currentBaseTarget, 4398046511104 / 240 / miningCoin->mining->currentBaseTarget, thread_time, miningCoin->mining->state == MiningState::DONE, miningCoin->mining->deadline }.detach();

			//TODO: why 'if not yet done'? where is it done? why only _32 here? why not all?
			//prepare for next round if not yet done
			if (!exit_flag && miningCoin->mining->state != MiningState::DONE) memcpy(&local_32, &global_32, sizeof(global_32));

			if (testmodeConfig.isEnabled)
			{
				bool anyDefined = false;
				if (miningCoin->testround2->check_scoop.has_value()) anyDefined = true;
				if (miningCoin->testround2->check_scoop_low.has_value()) anyDefined = true;
				if (miningCoin->testround2->check_scoop_high.has_value()) anyDefined = true;
				if (miningCoin->testround2->check_deadline.has_value()) anyDefined = true;

				bool anyDefinedAndSkipped = true;
				if (miningCoin->testround2->check_scoop.has_value() && !miningCoin->testround2->passed_scoop.has_value()) anyDefinedAndSkipped = false;
				if (miningCoin->testround2->check_scoop_low.has_value() && !miningCoin->testround2->passed_scoop_low.has_value()) anyDefinedAndSkipped = false;
				if (miningCoin->testround2->check_scoop_high.has_value() && !miningCoin->testround2->passed_scoop_high.has_value()) anyDefinedAndSkipped = false;
				if (miningCoin->testround2->check_deadline.has_value() && !miningCoin->testround2->passed_deadline.has_value()) anyDefinedAndSkipped = false;

				bool allDefinedHavePassed = true;
				if (miningCoin->testround2->passed_scoop.has_value() && !miningCoin->testround2->passed_scoop.value()) allDefinedHavePassed = false;
				if (miningCoin->testround2->passed_scoop_low.has_value() && !miningCoin->testround2->passed_scoop_low.value()) allDefinedHavePassed = false;
				if (miningCoin->testround2->passed_scoop_high.has_value() && !miningCoin->testround2->passed_scoop_high.value()) allDefinedHavePassed = false;
				if (miningCoin->testround2->passed_deadline.has_value() && !miningCoin->testround2->passed_deadline.value()) allDefinedHavePassed = false;

				if (!anyDefined)
				{
					gui->testmodeWarning(L"EMPTY");
					Log(L"TESTMODE: TEST EMPTY: no checks for this round, height: %llu, gensig: %s, baseTarget: %llu, account: %llu, nonce: %llu",
						miningCoin->testround1->height, miningCoin->testround1->signature.c_str(), miningCoin->testround1->baseTarget,
						miningCoin->testround2->assume_account, miningCoin->testround2->assume_nonce);
				}
				else if (!allDefinedHavePassed)
				{
					gui->testmodeError(L"FAILED");
					Log(L"TESTMODE: TEST FAILED: some checks have FAILED for this round, height: %llu, gensig: %s, baseTarget: %llu, account: %llu, nonce: %llu",
						miningCoin->testround1->height, miningCoin->testround1->signature.c_str(), miningCoin->testround1->baseTarget,
						miningCoin->testround2->assume_account, miningCoin->testround2->assume_nonce);
				}
				else if (!anyDefinedAndSkipped)
				{
					gui->testmodeError(L"PARTIAL");
					Log(L"TESTMODE: TEST PARTIAL: some checks were skipped for this round, height: %llu, gensig: %s, baseTarget: %llu, account: %llu, nonce: %llu",
						miningCoin->testround1->height, miningCoin->testround1->signature.c_str(), miningCoin->testround1->baseTarget,
						miningCoin->testround2->assume_account, miningCoin->testround2->assume_nonce);
				}
				else
				{
					gui->testmodeSuccess(L"PASSED");
					Log(L"TESTMODE: TEST PASSED: all checks have PASSED for this round, height: %llu, gensig: %s, baseTarget: %llu, account: %llu, nonce: %llu",
						miningCoin->testround1->height, miningCoin->testround1->signature.c_str(), miningCoin->testround1->baseTarget,
						miningCoin->testround2->assume_account, miningCoin->testround2->assume_nonce);
				}
			}
		}
	}

	if (testmodeConfig.isEnabled)
	{
		gui->testmodeFinished();
	}

	closeMiner();
	return 0;
}
