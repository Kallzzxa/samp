#include "ClientCmds.h"
#include "Client.h"
#include "Settings.h"
#include "UI/UI.h"
#include "Java/Java.h"
#include "Game/Game.h"
#include "Game/Hooks.h"
#include "Net/NetGame.h"
#include "Game/AudioStream.h"
#include "Game/CleoEngine.h"

#include <thread>
#include <cstdlib>

extern UI* pUI;
extern NetGame* pNetGame;
extern Game* pGame;
extern AudioStream* pAudioStream;

void cmdQuit(const std::string& params)
{
	if (pUI) {
		pUI->chat()->addInfoMessage("-> Exiting game...");
	}

	if (pNetGame) {
		if (pNetGame->GetRakClient()) {
			pNetGame->GetRakClient()->Disconnect(100);
		}
	}

	if (auto logger = spdlog::default_logger()) {
		logger->flush();
	}

	if (g_java) {
		g_java->exitGame();
	}
}

void cmdRcon(const std::string& params)
{
	if (params.empty()) {
		return;
	}

	uint8_t bytePacketId = ID_RCON_COMMAND;
	RakNet::BitStream bsCommand;
	bsCommand.Write(bytePacketId);
	auto dwCmdLen = (uint32_t) strlen(params.c_str());
	bsCommand.Write(dwCmdLen);
	bsCommand.Write(params.c_str(), dwCmdLen);
	pNetGame->GetRakClient()->Send(&bsCommand, HIGH_PRIORITY, RELIABLE, 0);
}

void cmdSave(const std::string& params)
{
	CPlayerPed* pPlayer = pGame->FindPlayerPed();
	FILE* fileOut;
	uint32_t dwVehicleID;
	float fZAngle;
	char szPath[0xFF];

	sprintf(szPath, "%sSAMP/savedpositions.txt", Client::gameDir());
	fileOut = fopen(szPath, "a");

	if (!fileOut) {
		if (pUI) {
			pUI->chat()->addInfoMessage("Save failed");
		}

		return;
	}

	// incar savepos

	if (pPlayer->IsInVehicle()) {

		sa::CVehicle* pVehicle = pPlayer->GetGtaVehicle();

		dwVehicleID = GamePool_Vehicle_GetIndex(pVehicle);
		ScriptCommand(&get_car_z_angle, dwVehicleID, &fZAngle);

		fprintf(fileOut, "AddStaticVehicle(%u, %.4f, %.4f, %.4f, %.4f, %u, %u); // %s\n",
				pVehicle->m_nModelIndex,
				pVehicle->GetPosition().x,
				pVehicle->GetPosition().y,
				pVehicle->GetPosition().z,
				fZAngle, pVehicle->m_nPrimaryColor,
				pVehicle->m_nSecondaryColor,
				params.c_str());

		fclose(fileOut);

		if (pUI) {
			pUI->chat()->addInfoMessage("-> Successfully saved vehicle position");
		}

		return;
	}

	// onfoot savepos

	sa::CPed* pActor = pPlayer->m_ped;
	ScriptCommand(&get_actor_z_angle, pPlayer->m_gtaId, &fZAngle);

	fprintf(fileOut, "AddPlayerClass(%u, %.4f, %.4f, %.4f, %.4f, 0, 0, 0, 0, 0, 0); // %s\n",
			pPlayer->GetModelIndex(),
			pActor->GetPosition().x,
			pActor->GetPosition().y,
			pActor->GetPosition().z,
			fZAngle,
			params.c_str());

	if (pUI) {
		pUI->chat()->addInfoMessage("-> Successfully saved onfoot position");
	}

	fclose(fileOut);
}

void cmdDL(const std::string& params)
{
	if (pUI) {
		pUI->chat()->addInfoMessage("-> DebugLabels %s", pUI->debuglabels()->visible() ? "OFF" : "ON");
		pUI->debuglabels()->setVisible(!pUI->debuglabels()->visible());
	}
}

void cmdDW(const std::string& params)
{
	if (pUI) {
		pUI->chat()->addInfoMessage("-> DeathWindow: %s", pUI->deathwindow()->visible() ? "OFF" : "ON");
		pUI->deathwindow()->setVisible(!pUI->deathwindow()->visible());
	}
}

void cmdClearMyChat(const std::string& params)
{
	if (!pUI) {
		return;
	}

	for (int i = 0; i < 25; i++) {
		pUI->chat()->addInfoMessage(" ");
	}
}

void cmdTimestamp(const std::string& params)
{
	if (pUI) {
		pUI->chat()->addInfoMessage("-> Timestamp %s", Chat::timestamp() ? "OFF" : "ON");
		Chat::setTimestamp(!Chat::timestamp());
		pUI->chat()->performLayout();
	}
}

void cmdHeadMove(const std::string& params)
{
	Settings::setHeadMove(!Settings::headMove());
	if (pUI) {
		pUI->chat()->addInfoMessage("-> HeadMove: %s", Settings::headMove() ? "ON" : "OFF");
	}
}

void cmdFreeCam(const std::string& params)
{
	Settings::setFreeCam(!Settings::freeCam());
	Settings::applyDynamicCameraAndFreeCam();
	if (pUI) {
		pUI->chat()->addInfoMessage("-> FreeCamera (自由镜头): %s", Settings::freeCam() ? "ON" : "OFF");
	}
}

void cmdHudEdit(const std::string& params)
{
	Hooks::openAdjustableHud();
}

void cmdPageSize(const std::string& params)
{
	if (!pUI) return;
	if (params.empty()) {
		pUI->chat()->addInfoMessage("-> Usage: /pagesize [5-20] (Current: %d)", Settings::chatstrings());
		return;
	}
	int lines = std::atoi(params.c_str());
	if (lines < 5 || lines > 20) {
		pUI->chat()->addInfoMessage("-> PageSize must be between 5 and 20 lines");
		return;
	}
	UISettings::setChatStrings(lines);
	pUI->chat()->setFixedSize(UISettings::chatSize());
	pUI->chat()->performLayout();
	pUI->chat()->addInfoMessage("-> Chat PageSize set to %d lines", lines);
}

void cmdScreenshot(const std::string& params)
{
	int mode = -1;
	if (params == "game" || params == "clean" || params == "1") {
		mode = 1;
	}
	else if (params == "hud" || params == "all" || params == "0") {
		mode = 0;
	}
	Hooks::requestScreenshot(mode);
}

void cmdCmds(const std::string& params)
{
	if (pUI) {
		pUI->chat()->addInfoMessage(" ");
		pUI->chat()->addInfoMessage("Client Commands:");
		pUI->chat()->addInfoMessage("/quit (/q) -> Exit Game");
		pUI->chat()->addInfoMessage("/ss (/screenshot) [hud|game] -> Take Screenshot (or F8 key)");
		pUI->chat()->addInfoMessage("/hudedit -> Open Touch Button Position Editor");
		pUI->chat()->addInfoMessage("/freecam -> Toggle Free Camera (No Auto-Center)");
		pUI->chat()->addInfoMessage("/headmove -> Toggle Character Head Rotation");
		pUI->chat()->addInfoMessage("/pagesize [5-20] -> Change Chat Line Count");
		pUI->chat()->addInfoMessage("/timestamp -> Toggle Chat Timestamp");
		pUI->chat()->addInfoMessage("/clearmychat -> Clear Local Chat Window");
		pUI->chat()->addInfoMessage("/save [note] -> Save Position to SAMP/savedpositions.txt");
		pUI->chat()->addInfoMessage("/dl -> Toggle 3D Entity Debug Labels");
		pUI->chat()->addInfoMessage("/dw -> Toggle Death / Kill Window");
		pUI->chat()->addInfoMessage("/rcon -> Send RCON Command (e.g. /rcon login [pass])");
		pUI->chat()->addInfoMessage(" ");
	}
}

void cmdJetpack(const std::string& params)
{
	if (pGame->FindPlayerPed()->IsInVehicle()) {
		pUI->chat()->addInfoMessage("-> You can't use jetpack while in vehicle");
		return;
	}

	pGame->FindPlayerPed()->StartJetpack();
	pUI->chat()->addInfoMessage("-> Jetpack started");
}

void cmdCleo(const std::string& params)
{
	if (!CleoEngine::isEnabled()) {
		if (pUI) pUI->chat()->addInfoMessage("-> CLEO 未开启（请在启动器设置/模组管理中启用 CLEO）");
		return;
	}
	CleoEngine::toggleMenu();
}

void ClientCmds::initialize()
{
	spdlog::info("Initializing client commands..");

	pUI->chat()->addClientCommand("cleo", cmdCleo);

	pUI->chat()->addClientCommand("ss", cmdScreenshot);
	pUI->chat()->addClientCommand("screenshot", cmdScreenshot);
	pUI->chat()->addClientCommand("hudedit", cmdHudEdit);
	pUI->chat()->addClientCommand("freecam", cmdFreeCam);
	pUI->chat()->addClientCommand("headmove", cmdHeadMove);
	pUI->chat()->addClientCommand("pagesize", cmdPageSize);
	pUI->chat()->addClientCommand("timestamp", cmdTimestamp);
	pUI->chat()->addClientCommand("clearmychat", cmdClearMyChat);
	pUI->chat()->addClientCommand("save", cmdSave);
	pUI->chat()->addClientCommand("clientcmds", cmdCmds);

	if (Client::offlineMode()) {
		pUI->chat()->addClientCommand("j", cmdJetpack);
		pUI->chat()->addClientCommand("q", cmdQuit);
		pUI->chat()->addClientCommand("quit", cmdQuit);
	}
	else {
		pUI->chat()->addClientCommand("q", cmdQuit);
		pUI->chat()->addClientCommand("quit", cmdQuit);
		pUI->chat()->addClientCommand("rcon", cmdRcon);
		pUI->chat()->addClientCommand("dl", cmdDL);
		pUI->chat()->addClientCommand("dw", cmdDW);
	}
}
