/*update
	Copyright 2013 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <dirent.h>
#include <pwd.h>

#include <string>
#include <sstream>
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../openrecoveryscript.hpp"
#include "../twrpDU.hpp"

#include <ctype.h>

#include "../adb_install.h"
#ifndef TW_NO_SCREEN_TIMEOUT
#include "blanktimer.hpp"
#endif

#include "../multirom.h"

extern "C" {
#include "../twcommon.h"
#include "../minuitwrp/minui.h"
#include "../variables.h"
#include "../twinstall.h"
#include "cutils/properties.h"
#include "../minadbd/adb.h"

#include "../mrominstaller.h"

int TWinstall_zip(const char* path, int* wipe_cache);
void run_script(const char *str1, const char *str2, const char *str3, const char *str4, const char *str5, const char *str6, const char *str7, int request_confirm);
int gui_console_only();
int gui_start();
};

#include "rapidxml.hpp"
#include "objects.hpp"

#ifndef TW_NO_SCREEN_TIMEOUT
extern blanktimer blankTimer;
#endif

void curtainClose(void);

GUIAction::GUIAction(xml_node<>* node)
	: GUIObject(node)
{
	xml_node<>* child;
	xml_node<>* actions;
	xml_attribute<>* attr;

	if (!node)  return;

	// First, get the action
	actions = node->first_node("actions");
	if (actions)	child = actions->first_node("action");
	else			child = node->first_node("action");

	if (!child) return;

	while (child)
	{
		Action action;

		attr = child->first_attribute("function");
		if (!attr)  return;

		action.mFunction = attr->value();
		action.mArg = child->value();
		mActions.push_back(action);

		child = child->next_sibling("action");
	}

	// Now, let's get either the key or region
	child = node->first_node("touch");
	if (child)
	{
		attr = child->first_attribute("key");
		if (attr)
		{
			std::vector<std::string> keys = TWFunc::Split_String(attr->value(), "+");
			for(size_t i = 0; i < keys.size(); ++i)
			{
				const int key = getKeyByName(keys[i]);
				mKeys[key] = false;
			}
		}
		else
		{
			attr = child->first_attribute("x");
			if (!attr)  return;
			mActionX = atol(attr->value());
			attr = child->first_attribute("y");
			if (!attr)  return;
			mActionY = atol(attr->value());
			attr = child->first_attribute("w");
			if (!attr)  return;
			mActionW = atol(attr->value());
			attr = child->first_attribute("h");
			if (!attr)  return;
			mActionH = atol(attr->value());
		}
	}
}

int GUIAction::NotifyTouch(TOUCH_STATE state, int x, int y)
{
	if (state == TOUCH_RELEASE)
		doActions();

	return 0;
}

int GUIAction::NotifyKey(int key, bool down)
{
	if (mKeys.empty())
		return 0;

	std::map<int, bool>::iterator itr = mKeys.find(key);
	if(itr == mKeys.end())
		return 0;

	bool prevState = itr->second;
	itr->second = down;

	// If there is only one key for this action, wait for key up so it
	// doesn't trigger with multi-key actions.
	// Else, check if all buttons are pressed, then consume their release events
	// so they don't trigger one-button actions and reset mKeys pressed status
	if(mKeys.size() == 1) {
		if(!down && prevState)
			doActions();
	} else if(down) {
		for(itr = mKeys.begin(); itr != mKeys.end(); ++itr) {
			if(!itr->second)
				return 0;
		}

		// Passed, all req buttons are pressed, reset them and consume release events
		HardwareKeyboard *kb = PageManager::GetHardwareKeyboard();
		for(itr = mKeys.begin(); itr != mKeys.end(); ++itr) {
			kb->ConsumeKeyRelease(itr->first);
			itr->second = false;
		}

		doActions();
	}

	return 0;
}

int GUIAction::NotifyVarChange(const std::string& varName, const std::string& value)
{
	GUIObject::NotifyVarChange(varName, value);

	if (varName.empty() && !isConditionValid() && mKeys.empty() && !mActionW)
		doActions();
	else if((varName.empty() || IsConditionVariable(varName)) && isConditionValid() && isConditionTrue())
		doActions();

	return 0;
}

void GUIAction::simulate_progress_bar(void)
{
	gui_print("Simulating actions...\n");
	for (int i = 0; i < 5; i++)
	{
		usleep(500000);
		DataManager::SetValue("ui_progress", i * 20);
	}
}

int GUIAction::flash_zip(std::string filename, std::string pageName, const int simulate, int* wipe_cache)
{
	int ret_val = 0;

	DataManager::SetValue("ui_progress", 0);

	if (filename.empty())
	{
		LOGERR("No file specified.\n");
		return -1;
	}

	// We're going to jump to this page first, like a loading page
	gui_changePage(pageName);

	int fd = -1;
	ZipArchive zip;

	if (!PartitionManager.Mount_By_Path(filename, true))
		return -1;

	if (mzOpenZipArchive(filename.c_str(), &zip))
	{
		LOGERR("Unable to open zip file.\n");
		return -1;
	}

	// Check the zip to see if it has a custom installer theme
	const ZipEntry* twrp = mzFindZipEntry(&zip, "META-INF/teamwin/twrp.zip");
	if (twrp != NULL)
	{
		unlink("/tmp/twrp.zip");
		fd = creat("/tmp/twrp.zip", 0666);
	}
	if (fd >= 0 && twrp != NULL &&
		mzExtractZipEntryToFile(&zip, twrp, fd) &&
		!PageManager::LoadPackage("install", "/tmp/twrp.zip", "main"))
	{
		mzCloseZipArchive(&zip);
		PageManager::SelectPackage("install");
		gui_changePage("main");
	}
	else
	{
		// In this case, we just use the default page
		mzCloseZipArchive(&zip);
		gui_changePage(pageName);
	}
	if (fd >= 0)
		close(fd);

	if (simulate) {
		simulate_progress_bar();
	} else {
		ret_val = TWinstall_zip(filename.c_str(), wipe_cache);

		// Now, check if we need to ensure TWRP remains installed...
		struct stat st;
		if (stat("/sbin/installTwrp", &st) == 0)
		{
			DataManager::SetValue("tw_operation", "Configuring TWRP");
			DataManager::SetValue("tw_partition", "");
			gui_print("Configuring TWRP...\n");
			if (TWFunc::Exec_Cmd("/sbin/installTwrp reinstall") < 0)
			{
				gui_print("Unable to configure TWRP with this kernel.\n");
			}
		}
	}

	// Done
	DataManager::SetValue("ui_progress", 100);
	DataManager::SetValue("ui_progress", 0);
	return ret_val;
}

int GUIAction::doActions()
{
	if (mActions.size() < 1)	return -1;
	if (mActions.size() == 1)
		return doAction(mActions.at(0), 0);

	// For multi-action, we always use a thread
	pthread_t t;
	pthread_attr_t tattr;

	if (pthread_attr_init(&tattr)) {
		LOGERR("Unable to pthread_attr_init\n");
		return -1;
	}
	if (pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE)) {
		LOGERR("Error setting pthread_attr_setdetachstate\n");
		return -1;
	}
	if (pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM)) {
		LOGERR("Error setting pthread_attr_setscope\n");
		return -1;
	}
	/*if (pthread_attr_setstacksize(&tattr, 524288)) {
		LOGERR("Error setting pthread_attr_setstacksize\n");
		return -1;
	}
	*/
	int ret = pthread_create(&t, &tattr, thread_start, this);
	if (ret) {
		LOGERR("Unable to create more threads for actions... continuing in same thread! %i\n", ret);
		thread_start(this);
	} else {
		if (pthread_join(t, NULL)) {
			LOGERR("Error joining threads\n");
		}
	}
	if (pthread_attr_destroy(&tattr)) {
		LOGERR("Failed to pthread_attr_destroy\n");
		return -1;
	}

	return 0;
}

void* GUIAction::thread_start(void *cookie)
{
	GUIAction* ourThis = (GUIAction*) cookie;

	DataManager::SetValue(TW_ACTION_BUSY, 1);

	if (ourThis->mActions.size() > 1)
	{
		std::vector<Action>::iterator iter;
		for (iter = ourThis->mActions.begin(); iter != ourThis->mActions.end(); iter++)
			ourThis->doAction(*iter, 1);
	}
	else
	{
		ourThis->doAction(ourThis->mActions.at(0), 1);
	}
	int check = 0;
	DataManager::GetValue("tw_background_thread_running", check);
	if (check == 0)
		DataManager::SetValue(TW_ACTION_BUSY, 0);
	return NULL;
}

void GUIAction::operation_start(const string operation_name)
{
	time(&Start);
	DataManager::SetValue(TW_ACTION_BUSY, 1);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("tw_operation", operation_name);
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 0);
}

void GUIAction::operation_end(const int operation_status, const int simulate)
{
	time_t Stop;
	int simulate_fail;
	DataManager::SetValue("ui_progress", 100);
	if (simulate) {
		DataManager::GetValue(TW_SIMULATE_FAIL, simulate_fail);
		if (simulate_fail != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	} else {
		if (operation_status != 0) {
			DataManager::SetValue("tw_operation_status", 1);
		}
		else {
			DataManager::SetValue("tw_operation_status", 0);
		}
	}
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
#ifndef TW_NO_SCREEN_TIMEOUT
	blankTimer.resetTimerAndUnblank();
#endif
	time(&Stop);
	if ((int) difftime(Stop, Start) > 10)
		DataManager::Vibrate("tw_action_vibrate");
}

int GUIAction::doAction(Action action, int isThreaded /* = 0 */)
{
	static string zip_queue[10];
	static int zip_queue_index;
	static pthread_t terminal_command;
	int simulate;

	std::string arg = gui_parse_text(action.mArg);

	std::string function = gui_parse_text(action.mFunction);

	DataManager::GetValue(TW_SIMULATE_ACTIONS, simulate);

	if (function == "reboot")
	{
			//curtainClose(); this sometimes causes a crash

		sync();
		DataManager::SetValue("tw_gui_done", 1);
		DataManager::SetValue("tw_reboot_arg", arg);

		return 0;
	}
	if (function == "home")
	{
		PageManager::SelectPackage("TWRP");
		gui_changePage("main");
		return 0;
	}

	if (function == "key")
	{
		const int key = getKeyByName(arg);
		PageManager::NotifyKey(key, true);
		PageManager::NotifyKey(key, false);
		return 0;
	}

	if (function == "page") {
		std::string page_name = gui_parse_text(arg);
		return gui_changePage(page_name);
	}

	if (function == "reload") {
		operation_start("Reload Theme");
		int ret_val = !TWFunc::reloadTheme();
		operation_end(ret_val, simulate);
		return 0;
	}

	if(function == "rotation") {
		int rot = atoi(arg.c_str());

		if(rot == gr_get_rotation())
			return 0;

		operation_start("Rotation");
		int res = gui_rotate(rot);
		operation_end(res != 0, simulate);
		return 0;
	}

	if (function == "readBackup")
	{
		string Restore_Name;
		DataManager::GetValue("tw_restore", Restore_Name);
		PartitionManager.Set_Restore_Files(Restore_Name);
		return 0;
	}

	if (function == "set")
	{
		if (arg.find('=') != string::npos)
		{
			string varName = arg.substr(0, arg.find('='));
			string value = arg.substr(arg.find('=') + 1, string::npos);

			DataManager::GetValue(value, value);
			DataManager::SetValue(varName, value);
		}
		else
			DataManager::SetValue(arg, "1");
		return 0;
	}
	if (function == "clear")
	{
		DataManager::SetValue(arg, "0");
		return 0;
	}

	if (function == "mount") {
		if (arg == "usb") {
			DataManager::SetValue(TW_ACTION_BUSY, 1);
			if (!simulate)
				PartitionManager.usb_storage_enable();
			else
				gui_print("Simulating actions...\n");
		} else if (!simulate) {
			PartitionManager.Mount_By_Path(arg, true);
		} else
			gui_print("Simulating actions...\n");
		return 0;
	}

	if (function == "umount" || function == "unmount") {
		if (arg == "usb") {
			if (!simulate)
				PartitionManager.usb_storage_disable();
			else
				gui_print("Simulating actions...\n");
			DataManager::SetValue(TW_ACTION_BUSY, 0);
		} else if (!simulate) {
			PartitionManager.UnMount_By_Path(arg, true);
		} else
			gui_print("Simulating actions...\n");
		return 0;
	}

	if (function == "restoredefaultsettings")
	{
		operation_start("Restore Defaults");
		if (simulate) // Simulated so that people don't accidently wipe out the "simulation is on" setting
			gui_print("Simulating actions...\n");
		else {
			DataManager::ResetDefaults();
			PartitionManager.Update_System_Details();
			PartitionManager.Mount_Current_Storage(true);
		}
		operation_end(0, simulate);
		return 0;
	}

	if (function == "copylog")
	{
		operation_start("Copy Log");
		if (!simulate)
		{
			string dst;
			PartitionManager.Mount_Current_Storage(true);
			dst = DataManager::GetCurrentStoragePath() + "/recovery.log";
			TWFunc::copy_file("/tmp/recovery.log", dst.c_str(), 0755);
			sync();
			gui_print("Copied recovery log to %s.\n", DataManager::GetCurrentStoragePath().c_str());
		} else
			simulate_progress_bar();
		operation_end(0, simulate);
		return 0;
	}

	if (function == "compute" || function == "addsubtract")
	{
		if (arg.find("+") != string::npos)
		{
			string varName = arg.substr(0, arg.find('+'));
			string string_to_add = arg.substr(arg.find('+') + 1, string::npos);
			int amount_to_add = atoi(string_to_add.c_str());
			int value;

			DataManager::GetValue(varName, value);
			DataManager::SetValue(varName, value + amount_to_add);
			return 0;
		}
		if (arg.find("-") != string::npos)
		{
			string varName = arg.substr(0, arg.find('-'));
			string string_to_subtract = arg.substr(arg.find('-') + 1, string::npos);
			int amount_to_subtract = atoi(string_to_subtract.c_str());
			int value;

			DataManager::GetValue(varName, value);
			value -= amount_to_subtract;
			if (value <= 0)
				value = 0;
			DataManager::SetValue(varName, value);
			return 0;
		}
		if (arg.find("*") != string::npos)
		{
			string varName = arg.substr(0, arg.find('*'));
			string multiply_by_str = gui_parse_text(arg.substr(arg.find('*') + 1, string::npos));
			int multiply_by = atoi(multiply_by_str.c_str());
			int value;

			DataManager::GetValue(varName, value);
			DataManager::SetValue(varName, value*multiply_by);
			return 0;
		}
		if (arg.find("/") != string::npos)
		{
			string varName = arg.substr(0, arg.find('/'));
			string divide_by_str = gui_parse_text(arg.substr(arg.find('/') + 1, string::npos));
			int divide_by = atoi(divide_by_str.c_str());
			int value;

			if(divide_by != 0)
			{
				DataManager::GetValue(varName, value);
				DataManager::SetValue(varName, value/divide_by);
			}
			return 0;
		}
		LOGERR("Unable to perform compute '%s'\n", arg.c_str());
		return -1;
	}

	if (function == "setguitimezone")
	{
		string SelectedZone;
		DataManager::GetValue(TW_TIME_ZONE_GUISEL, SelectedZone); // read the selected time zone into SelectedZone
		string Zone = SelectedZone.substr(0, SelectedZone.find(';')); // parse to get time zone
		string DSTZone = SelectedZone.substr(SelectedZone.find(';') + 1, string::npos); // parse to get DST component

		int dst;
		DataManager::GetValue(TW_TIME_ZONE_GUIDST, dst); // check wether user chose to use DST

		string offset;
		DataManager::GetValue(TW_TIME_ZONE_GUIOFFSET, offset); // pull in offset

		string NewTimeZone = Zone;
		if (offset != "0")
			NewTimeZone += ":" + offset;

		if (dst != 0)
			NewTimeZone += DSTZone;

		DataManager::SetValue(TW_TIME_ZONE_VAR, NewTimeZone);
		DataManager::update_tz_environment_variables();
		return 0;
	}

	if (function == "togglestorage") {
		LOGERR("togglestorage action was deprecated from TWRP\n");
		return 0;
	}

	if (function == "overlay")
		return gui_changeOverlay(arg);

	if (function == "queuezip")
	{
		if (zip_queue_index >= 10) {
			gui_print("Maximum zip queue reached!\n");
			return 0;
		}
		DataManager::GetValue("tw_filename", zip_queue[zip_queue_index]);
		if (strlen(zip_queue[zip_queue_index].c_str()) > 0) {
			zip_queue_index++;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}

	if (function == "cancelzip")
	{
		if (zip_queue_index <= 0) {
			gui_print("Minimum zip queue reached!\n");
			return 0;
		} else {
			zip_queue_index--;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}

	if (function == "queueclear")
	{
		zip_queue_index = 0;
		DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		return 0;
	}

	if (function == "sleep")
	{
		operation_start("Sleep");
		usleep(atoi(arg.c_str()));
		operation_end(0, simulate);
		return 0;
	}

	if (function == "appenddatetobackupname")
	{
		operation_start("AppendDateToBackupName");
		string Backup_Name;
		DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
		Backup_Name += TWFunc::Get_Current_Date();
		if (Backup_Name.size() > MAX_BACKUP_NAME_LEN)
			Backup_Name.resize(MAX_BACKUP_NAME_LEN);
		DataManager::SetValue(TW_BACKUP_NAME, Backup_Name);
		operation_end(0, simulate);
		return 0;
	}

	if (function == "generatebackupname")
	{
		operation_start("GenerateBackupName");
		TWFunc::Auto_Generate_Backup_Name();
		operation_end(0, simulate);
		return 0;
	}
	if (function == "checkpartitionlist") {
		string Wipe_List, wipe_path;
		int count = 0;

		DataManager::GetValue("tw_wipe_list", Wipe_List);
		LOGINFO("checkpartitionlist list '%s'\n", Wipe_List.c_str());
		if (!Wipe_List.empty()) {
			size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
			while (end_pos != string::npos && start_pos < Wipe_List.size()) {
				wipe_path = Wipe_List.substr(start_pos, end_pos - start_pos);
				LOGINFO("checkpartitionlist wipe_path '%s'\n", wipe_path.c_str());
				if (wipe_path == "/and-sec" || wipe_path == "DALVIK" || wipe_path == "INTERNAL") {
					// Do nothing
				} else {
					count++;
				}
				start_pos = end_pos + 1;
				end_pos = Wipe_List.find(";", start_pos);
			}
			DataManager::SetValue("tw_check_partition_list", count);
		} else {
			DataManager::SetValue("tw_check_partition_list", 0);
		}
		return 0;
	}
	if (function == "getpartitiondetails") {
		string Wipe_List, wipe_path;
		int count = 0;

		DataManager::GetValue("tw_wipe_list", Wipe_List);
		LOGINFO("getpartitiondetails list '%s'\n", Wipe_List.c_str());
		if (!Wipe_List.empty()) {
			size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
			while (end_pos != string::npos && start_pos < Wipe_List.size()) {
				wipe_path = Wipe_List.substr(start_pos, end_pos - start_pos);
				LOGINFO("getpartitiondetails wipe_path '%s'\n", wipe_path.c_str());
				if (wipe_path == "/and-sec" || wipe_path == "DALVIK" || wipe_path == "INTERNAL") {
					// Do nothing
				} else {
					DataManager::SetValue("tw_partition_path", wipe_path);
					break;
				}
				start_pos = end_pos + 1;
				end_pos = Wipe_List.find(";", start_pos);
			}
			if (!wipe_path.empty()) {
				TWPartition* Part = PartitionManager.Find_Partition_By_Path(wipe_path);
				if (Part) {
					unsigned long long mb = 1048576;

					DataManager::SetValue("tw_partition_name", Part->Display_Name);
					DataManager::SetValue("tw_partition_mount_point", Part->Mount_Point);
					DataManager::SetValue("tw_partition_file_system", Part->Current_File_System);
					DataManager::SetValue("tw_partition_size", Part->Size / mb);
					DataManager::SetValue("tw_partition_used", Part->Used / mb);
					DataManager::SetValue("tw_partition_free", Part->Free / mb);
					DataManager::SetValue("tw_partition_backup_size", Part->Backup_Size / mb);
					DataManager::SetValue("tw_partition_removable", Part->Removable);
					DataManager::SetValue("tw_partition_is_present", Part->Is_Present);

					if (Part->Can_Repair())
						DataManager::SetValue("tw_partition_can_repair", 1);
					else
						DataManager::SetValue("tw_partition_can_repair", 0);
					if (TWFunc::Path_Exists("/sbin/mkdosfs"))
						DataManager::SetValue("tw_partition_vfat", 1);
					else
						DataManager::SetValue("tw_partition_vfat", 0);
					if (TWFunc::Path_Exists("/sbin/mkfs.exfat"))
						DataManager::SetValue("tw_partition_exfat", 1);
					else
						DataManager::SetValue("tw_partition_exfat", 0);
					if (TWFunc::Path_Exists("/sbin/mkfs.f2fs"))
						DataManager::SetValue("tw_partition_f2fs", 1);
					else
						DataManager::SetValue("tw_partition_f2fs", 0);
					if (TWFunc::Path_Exists("/sbin/mke2fs"))
						DataManager::SetValue("tw_partition_ext", 1);
					else
						DataManager::SetValue("tw_partition_ext", 0);
					return 0;
				} else {
					LOGERR("Unable to locate partition: '%s'\n", wipe_path.c_str());
				}
			}
		}
		DataManager::SetValue("tw_partition_name", "");
		DataManager::SetValue("tw_partition_file_system", "");
		return 0;
	}

	if (function == "screenshot")
	{
		time_t tm;
		char path[256];
		int path_len;
		uid_t uid = -1;
		gid_t gid = -1;

		struct passwd *pwd = getpwnam("media_rw");
		if(pwd) {
			uid = pwd->pw_uid;
			gid = pwd->pw_gid;
		}

		const std::string storage = DataManager::GetCurrentStoragePath();
		if(PartitionManager.Is_Mounted_By_Path(storage)) {
			snprintf(path, sizeof(path), "%s/Pictures/Screenshots/", storage.c_str());
		} else {
			strcpy(path, "/tmp/");
		}

		if(!TWFunc::Create_Dir_Recursive(path, 0666, uid, gid))
			return 0;

		tm = time(NULL);
		path_len = strlen(path);

		// Screenshot_2014-01-01-18-21-38.png
		strftime(path+path_len, sizeof(path)-path_len, "Screenshot_%Y-%m-%d-%H-%M-%S.png", localtime(&tm));

		gui_setRenderEnabled(0);
		int res = gr_save_screenshot(path);
		gui_setRenderEnabled(1);

		if(res == 0) {
			chmod(path, 0666);
			chown(path, uid, gid);

			gui_print("Screenshot was saved to %s\n", path);

			// blink to notify that the screenshow was taken
			gr_color(255, 255, 255, 255);
			gr_fill(0, 0, gr_fb_width(), gr_fb_height());
			gr_flip();
			gui_forceRender();
		} else {
			LOGERR("Failed to take a screenshot!\n");
		}
		return 0;
	}

	if (function == "setbrightness")
	{
		return TWFunc::Set_Brightness(arg);
	}

	if (function == "multirom")
	{
		if(MultiROM::folderExists())
			return gui_changePage("multirom_main");
		else
		{
			DataManager::SetValue("tw_mrom_title", "MultiROM is not installed!");
			DataManager::SetValue("tw_mrom_text1", "/data/media/multirom not found.");
			DataManager::SetValue("tw_mrom_text2", "/data/media/0/multirom not found.");
			DataManager::SetValue("tw_mrom_back", "advanced");
			return gui_changePage("multirom_msg");
		}
	}

	if (function == "multirom_reset_roms_paths")
	{
		MultiROM::setRomsPath(INTERNAL_MEM_LOC_TXT);
		DataManager::SetValue("tw_multirom_folder", MultiROM::getRomsPath());
		DataManager::SetValue("tw_multirom_install_loc", INTERNAL_MEM_LOC_TXT);
		return 0;
	}

	if (function == "multirom_rename")
	{
		std::string new_name = arg;
		TWFunc::trim(new_name);
		MultiROM::move(DataManager::GetStrValue("tw_multirom_rom_name"), new_name);
		return gui_changePage("multirom_list");
	}

	if (function == "multirom_manage")
	{
		std::string name = DataManager::GetStrValue("tw_multirom_rom_name");
		int type = MultiROM::getType(name);
		DataManager::SetValue("tw_multirom_is_android", (M(type) & MASK_ANDROID) != 0);
		DataManager::SetValue("tw_multirom_is_ubuntu", (M(type) & MASK_UBUNTU) != 0);
		DataManager::SetValue("tw_multirom_is_touch", (M(type) & MASK_UTOUCH) != 0);
		DataManager::SetValue("tw_multirom_is_sailfish", (M(type) & MASK_SAILFISH) != 0);
		if((M(type) & MASK_ANDROID) != 0)
		{
			std::string path = MultiROM::getRomsPath() + "/" + name + "/boot.img";
			DataManager::SetValue("tw_multirom_has_bootimg", access(path.c_str(), F_OK) >= 0);
		}
		DataManager::SetValue("tw_multirom_has_fw_partition", MultiROM::hasFirmwareDev());
		if(MultiROM::hasFirmwareDev())
		{
			std::string fw_file = MultiROM::getRomsPath() + DataManager::GetStrValue("tw_multirom_rom_name") + "/firmware.img";
			DataManager::SetValue("tw_multirom_has_fw_image", int(access(fw_file.c_str(), F_OK) >= 0));
		}
		return gui_changePage("multirom_manage");
	}

	if (function == "multirom_settings")
	{
		MultiROM::config cfg = MultiROM::loadConfig();

		if(cfg.auto_boot_type & MROM_AUTOBOOT_CHECK_KEYS)
			DataManager::SetValue("tw_multirom_auto_boot_trigger", MROM_AUTOBOOT_TRIGGER_KEYS);
		else if(cfg.auto_boot_seconds > 0)
			DataManager::SetValue("tw_multirom_auto_boot_trigger", MROM_AUTOBOOT_TRIGGER_TIME);
		else
			DataManager::SetValue("tw_multirom_auto_boot_trigger", MROM_AUTOBOOT_TRIGGER_DISABLED);

		if(cfg.auto_boot_seconds <= 0)
			DataManager::SetValue("tw_multirom_delay", 5);
		else
			DataManager::SetValue("tw_multirom_delay", cfg.auto_boot_seconds);
		DataManager::SetValue("tw_multirom_current", cfg.current_rom);
		DataManager::SetValue("tw_multirom_auto_boot_rom", cfg.auto_boot_rom);
		DataManager::SetValue("tw_multirom_auto_boot_type", (cfg.auto_boot_type & MROM_AUTOBOOT_LAST));
		DataManager::SetValue("tw_multirom_colors", cfg.colors);
		DataManager::SetValue("tw_multirom_brightness", cfg.brightness);
		DataManager::SetValue("tw_multirom_enable_adb", cfg.enable_adb);
		DataManager::SetValue("tw_multirom_hide_internal", cfg.hide_internal);
		DataManager::SetValue("tw_multirom_int_display_name", cfg.int_display_name);
		DataManager::SetValue("tw_multirom_rotation", cfg.rotation);
		DataManager::SetValue("tw_multirom_force_generic_fb", cfg.force_generic_fb);
		DataManager::SetValue("tw_anim_duration_coef_pct", cfg.anim_duration_coef_pct);

		DataManager::SetValue("tw_multirom_unrecognized_opts", cfg.unrecognized_opts);

		DataManager::SetValue("tw_multirom_roms", MultiROM::listRoms());
		return gui_changePage("multirom_settings");
	}

	if (function == "multirom_settings_save")
	{
		MultiROM::config cfg;
		cfg.current_rom = DataManager::GetStrValue("tw_multirom_current");
		cfg.auto_boot_type = DataManager::GetIntValue("tw_multirom_auto_boot_type");
		switch(DataManager::GetIntValue("tw_multirom_auto_boot_trigger"))
		{
			case MROM_AUTOBOOT_TRIGGER_DISABLED:
				cfg.auto_boot_seconds = 0;
				break;
			case MROM_AUTOBOOT_TRIGGER_TIME:
				cfg.auto_boot_seconds = DataManager::GetIntValue("tw_multirom_delay");
				break;
			case MROM_AUTOBOOT_TRIGGER_KEYS:
				cfg.auto_boot_type |= MROM_AUTOBOOT_CHECK_KEYS;
				break;
		}
		cfg.auto_boot_rom = DataManager::GetStrValue("tw_multirom_auto_boot_rom");
		cfg.colors = DataManager::GetIntValue("tw_multirom_colors");
		cfg.brightness = DataManager::GetIntValue("tw_multirom_brightness");
		cfg.enable_adb = DataManager::GetIntValue("tw_multirom_enable_adb");
		cfg.hide_internal = DataManager::GetIntValue("tw_multirom_hide_internal");
		cfg.int_display_name = DataManager::GetStrValue("tw_multirom_int_display_name");
		cfg.rotation = DataManager::GetIntValue("tw_multirom_rotation");
		cfg.force_generic_fb = DataManager::GetIntValue("tw_multirom_force_generic_fb");
		cfg.anim_duration_coef_pct = DataManager::GetIntValue("tw_anim_duration_coef_pct");

		cfg.unrecognized_opts = DataManager::GetStrValue("tw_multirom_unrecognized_opts");

		MultiROM::saveConfig(cfg);
		return 0;
	}

	if (function == "multirom_add")
	{
		DataManager::SetValue("tw_multirom_install_loc_list", MultiROM::listInstallLocations());
		DataManager::SetValue("tw_multirom_install_loc", INTERNAL_MEM_LOC_TXT);
		MultiROM::updateSupportedSystems();
		return gui_changePage("multirom_add");
	}

	if (function == "multirom_add_second")
	{
		switch(DataManager::GetIntValue("tw_multirom_type"))
		{
			case 1:
				return gui_changePage("multirom_add_source");
			case 5:
				DataManager::SetValue("tw_sailfish_filename_base", "");
				DataManager::SetValue("tw_sailfish_filename_rootfs", "");
				return gui_changePage("multirom_add_sailfish");
			default:
				return gui_changePage("multirom_add_select");
		}
	}

	if (function == "multirom_add_file_selected")
	{
		std::string loc = DataManager::GetStrValue("tw_multirom_install_loc");
		bool images = MultiROM::installLocNeedsImages(loc);
		int type = DataManager::GetIntValue("tw_multirom_type");

		MultiROM::clearBaseFolders();

		if(type == 1 || type == 2 || type == 5)
		{
			switch(type)
			{
				case 1: // Android
					MultiROM::addBaseFolder("data", DATA_IMG_MINSIZE, DATA_IMG_DEFSIZE);
					MultiROM::addBaseFolder("system", SYS_IMG_MINSIZE, SYS_IMG_DEFSIZE);
					MultiROM::addBaseFolder("cache", CACHE_IMG_MINSIZE, CACHE_IMG_DEFSIZE);
					break;
				case 2: // Ubuntu dekstop
					MultiROM::addBaseFolder("root", UB_DATA_IMG_MINSIZE, UB_DATA_IMG_DEFSIZE);
					break;
				case 5: // SailfishOS
					MultiROM::addBaseFolder("data", SAILFISH_DATA_IMG_MINSIZE, SAILFISH_DATA_IMG_DEFSIZE);
					MultiROM::addBaseFolder("system", SYS_IMG_MINSIZE, SYS_IMG_DEFSIZE);
					MultiROM::addBaseFolder("cache", CACHE_IMG_MINSIZE, CACHE_IMG_DEFSIZE);
					break;
			}

			MultiROM::updateImageVariables();

			if(images)
				return gui_changePage("multirom_add_image_size");
			else
				return gui_changePage("multirom_add_start_process");
		}
		else if(type == 3)
		{
			DataManager::SetValue("tw_mrom_back", "multirom_add");
			DataManager::SetValue("tw_mrom_text2", "");

			std:string ex;
			MROMInstaller *i = new MROMInstaller();

			DataManager::SetValue("tw_mrom_title", "Bad installer");
			if(!(ex = i->open(DataManager::GetStrValue("tw_filename"))).empty())
				return i->destroyWithErrorMsg(ex);

			DataManager::SetValue("tw_mrom_title", "Unsupported device");
			if(!(ex = i->checkDevices()).empty())
				return i->destroyWithErrorMsg(ex);

			DataManager::SetValue("tw_mrom_title", "Old MultiROM");
			if(!(ex = i->checkVersion()).empty())
				return i->destroyWithErrorMsg(ex);

			DataManager::SetValue("tw_mrom_title", "Unsupported install location");
			if(!(ex = i->setInstallLoc(loc, images)).empty())
				return i->destroyWithErrorMsg(ex);
			
			if(!(ex = i->parseBaseFolders(loc.find("ntfs") != std::string::npos)).empty())
				return i->destroyWithErrorMsg(ex);

			MultiROM::updateImageVariables();
			MultiROM::setInstaller(i);

			if(images)
				return gui_changePage("multirom_add_image_size");
			else
				return gui_changePage("multirom_add_start_process");
		}
	}

	if (function == "multirom_change_img_size")
	{
		DataManager::SetValue("tw_multirom_image_too_small", 0);
		DataManager::SetValue("tw_multirom_image_too_big", 0);
		DataManager::SetValue("tw_multirom_image_name", arg);

		base_folder *b = MultiROM::getBaseFolder(arg);
		if(b != NULL)
			DataManager::SetValue("tw_multirom_image_size", b->size);

		return gui_changePage("multirom_change_img_size");
	}

	if (function == "multirom_change_img_size_act")
	{
		int value = DataManager::GetIntValue("tw_multirom_image_size");

		base_folder *b = MultiROM::getBaseFolder(DataManager::GetStrValue("tw_multirom_image_name"));
		if(!b)
			return gui_changePage("multirom_add_image_size");

		DataManager::SetValue("tw_multirom_image_too_small", 0);
		DataManager::SetValue("tw_multirom_image_too_big", 0);

		if(value < b->min_size)
		{
			DataManager::SetValue("tw_multirom_image_too_small", 1);
			DataManager::SetValue("tw_multirom_min_size", b->min_size);
			return gui_changePage("multirom_change_img_size");
		}

		if(value > 4095 &&
			DataManager::GetStrValue("tw_multirom_install_loc").find("(vfat") != std::string::npos)
		{
			DataManager::SetValue("tw_multirom_image_too_big", 1);
			return gui_changePage("multirom_change_img_size");
		}

		b->size = value;
		MultiROM::updateImageVariables();
		return gui_changePage("multirom_add_image_size");
	}

	if (function == "multirom_set_list_loc")
	{
		DataManager::SetValue("tw_multirom_install_loc_list", MultiROM::listInstallLocations());
		return gui_changePage("multirom_set_list_loc");
	}

	if (function == "multirom_list_loc_selected")
	{
		std::string loc = DataManager::GetStrValue("tw_multirom_install_loc");
		if(!MultiROM::setRomsPath(loc))
			MultiROM::setRomsPath(INTERNAL_MEM_LOC_TXT);
		DataManager::SetValue("tw_multirom_folder", MultiROM::getRomsPath());
		return gui_changePage("multirom_list");
	}

	if(function == "multirom_exit_backup")
	{
		if(DataManager::GetIntValue("multirom_do_backup") == 1)
		{
			operation_start("Restoring default backup settings");
			MultiROM::deinitBackup();
			operation_end(0, simulate);
		}
		else if(arg == "multirom_manage")
			arg = "main";

		return gui_changePage(arg);
	}

	if(function == "multirom_create_internal_rom_name")
	{
		std::string name = TWFunc::getROMName();
		if(name.size() > MAX_ROM_NAME)
			name.resize(MAX_ROM_NAME);
		else if(name.empty())
			name = "unknown";

		TWFunc::stringReplace(name, ' ', '_');

		std::string roms = MultiROM::getRomsPath();
		for(char i = '1'; TWFunc::Path_Exists(roms + name) && i <= '9'; ++i)
			name.replace(name.size()-1, 1, 1, i);

		DataManager::SetValue(arg, name);
		return 0;
	}

	if (function == "multirom_list_roms_for_swap")
	{
		const int mask = MASK_ANDROID & (~M(ROM_INTERNAL_PRIMARY));
		DataManager::SetValue(arg, MultiROM::listRoms(mask, true));
		return 0;
	}

	if (isThreaded)
	{
		if (function == "timeout")
		{
#ifndef TW_NO_SCREEN_TIMEOUT
			blankTimer.blankScreen();
			return 0;
#endif
		}
	
		if (function == "multirom_delete")
		{
			int op_status = 0;
			operation_start("Delete ROM");
			if(!MultiROM::erase(DataManager::GetStrValue("tw_multirom_rom_name")))
				op_status = 1;
			PartitionManager.Update_System_Details();
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_flash_zip")
		{
			operation_start("Flashing");
			int op_status = 0;

			std::string name = DataManager::GetStrValue("tw_multirom_rom_name");
			std::string boot = MultiROM::getRomsPath() + name + "/boot.img";
			int had_boot = access(boot.c_str(), F_OK) >= 0;

			if (!MultiROM::flashZip(name, DataManager::GetStrValue("tw_filename")))
				op_status = 1;

			if(!had_boot && MultiROM::compareFiles(MultiROM::getBootDev().c_str(), boot.c_str()))
				unlink(boot.c_str());
			else if(op_status == 0)
			{
				DataManager::SetValue("tw_multirom_share_kernel", 0);
				if(!MultiROM::extractBootForROM(MultiROM::getRomsPath() + name))
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_flash_zip_sailfish")
		{
			operation_start("Flashing");
			int op_status = 0;

			std::string name = DataManager::GetStrValue("tw_multirom_rom_name");
			std::string root = MultiROM::getRomsPath() + name;

			if(rename((root + "/data/.stowaways/sailfishos/system").c_str(), (root + "/system").c_str()) < 0)
				gui_print("/system move failed %s", strerror(errno));


			if (!MultiROM::flashZip(name, DataManager::GetStrValue("tw_filename")))
				op_status = 1;

			if(rename((root + "/system").c_str(), (root + "/data/.stowaways/sailfishos/system").c_str()) < 0)
				gui_print("/system move failed %s", strerror(errno));

			if(!MultiROM::sailfishProcessBoot(root))
				op_status = 1;

			if(!MultiROM::sailfishProcess(root, name))
				op_status = 1;

			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_inject")
		{
			operation_start("Injecting");
			int op_status = 0;
			std::string path = DataManager::GetStrValue("tw_filename");
			if(DataManager::GetIntValue("tw_multirom_add_bootimg"))
				op_status = MultiROM::copyBoot(path, DataManager::GetStrValue("tw_multirom_rom_name"));

			if(!op_status)
				op_status = !MultiROM::injectBoot(path);
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_inject_curr_boot")
		{
			operation_start("Injecting");
			int op_status = !MultiROM::folderExists();
			if(op_status)
				gui_print("MultiROM is not installed!\n");
			else
				op_status = !MultiROM::injectBoot(MultiROM::getBootDev());
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_add_rom")
		{
			operation_start("Installing");

			int op_status = !MultiROM::addROM(DataManager::GetStrValue("tw_filename"),
											  DataManager::GetIntValue("tw_multirom_type"),
											  DataManager::GetStrValue("tw_multirom_install_loc"));
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_ubuntu_patch_init")
		{
			operation_start("Patching");
			int op_status = !MultiROM::patchInit(DataManager::GetStrValue("tw_multirom_rom_name"));
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_touch_patch_init")
		{
			operation_start("Patching");
			int op_status = 1;
			std::string root = MultiROM::getRomsPath() + DataManager::GetStrValue("tw_multirom_rom_name") + "/";
			if(access((root + "/boot.img").c_str(), F_OK) >= 0)
			{
				std::string type;
				if(access((root + "/data/system.img").c_str(), F_OK) >= 0)
					type = "ubuntu-touch-sysimage-init";
				else
					type = "ubuntu-touch-init";

				gui_print("Patching ubuntu with %s\n", type.c_str());
				op_status = !MultiROM::ubuntuTouchProcessBoot(root, type.c_str());
				if(op_status == 0)
					op_status = !MultiROM::ubuntuTouchProcess(root, DataManager::GetStrValue("tw_multirom_rom_name"));
			}
			else
				LOGERR("This ubuntu installation does not have boot.img, it can't be patched.\n");
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_wipe")
		{
			operation_start("Wiping");
			int op_status = !MultiROM::wipe(DataManager::GetStrValue("tw_multirom_rom_name"),
											DataManager::GetStrValue("tw_multirom_wipe"));
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_disable_flash_kernel")
		{
			operation_start("working");
			int op_status = !MultiROM::disableFlashKernelAct(DataManager::GetStrValue("tw_multirom_rom_name"),
															 DataManager::GetStrValue("tw_multirom_install_loc"));
			operation_end(op_status, simulate);
			return 0;
		}

		if(function == "multirom_rm_bootimg")
		{
			operation_start("working");
			std::string cmd = "rm \"" + MultiROM::getRomsPath() + "/" + DataManager::GetStrValue("tw_multirom_rom_name") + "/boot.img\"";
			int op_status = (system(cmd.c_str()) != 0);
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_backup_rom")
		{
			operation_start("Changing mountpoints for backup");
			int op_status = !MultiROM::initBackup(DataManager::GetStrValue("tw_multirom_rom_name"));
			operation_end(op_status, simulate);
			if(op_status == 0)
				return gui_changePage("backup");
			else
			{
				DataManager::SetValue("tw_mrom_title", "Failed to prepare ROM for backup!");
				DataManager::SetValue("tw_mrom_text1", "See /tmp/recovery.log for more details.");
				DataManager::SetValue("tw_mrom_text2", "");
				DataManager::SetValue("tw_mrom_back", "multirom_manage");
				return gui_changePage("multirom_msg");
			}
		}

		if(function == "multirom_sideload")
		{
			int ret = 0;

			operation_start("Sideload");
			string Sideload_File;

			if (!PartitionManager.Mount_Current_Storage(true)) {
				operation_end(1, simulate);
				return 0;
			}
			Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
			if (TWFunc::Path_Exists(Sideload_File))
				unlink(Sideload_File.c_str());

			gui_print("Starting ADB sideload feature...\n");
			ret = apply_from_adb(Sideload_File.c_str());
			DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui now that the zip install is going to start
			if (ret != 0) {
				operation_end(1, simulate);
				return 0;
			} else {
				operation_end(0, simulate);

				DataManager::SetValue("tw_filename", Sideload_File);
				DataManager::SetValue("tw_mrom_sideloaded", Sideload_File);
				gui_changePage(DataManager::GetStrValue("tw_mrom_next_page"));
				return 0;
			}
		}

		if(function == "multirom_swap_calc_space")
		{
			static const char *parts[] = { "/cache", "/system", "/data" };
			TWPartition *p;
			unsigned long long int_size = 0, int_data_size = 0, sec_data_size = 0;
			unsigned long long needed = 0, free = 0;

			std::string swap_rom = DataManager::GetStrValue("tw_multirom_swap_rom");
			int type = DataManager::GetIntValue("tw_multirom_swap_type");

			operation_start("CalcSpace");

			PartitionManager.Update_System_Details();

			p = PartitionManager.Find_Partition_By_Path(MultiROM::getRomsPath());
			if(!p)
			{
				LOGINFO("multirom_swap_calc_space: failed to find partition for ROMs!\n");
				operation_end(1, simulate);
				return 0;
			}

			free = p->GetSizeFree();

			for(size_t i = 0; i < sizeof(parts)/sizeof(parts[0]); ++i)
			{
				p = PartitionManager.Find_Partition_By_Path(parts[i]);
				if(!p)
				{
					LOGINFO("multirom_swap_calc_space: failed to get %s!\n", parts[i]);
					operation_end(1, simulate);
					return 0;
				}

				int_size += p->GetSizeBackup();
				if(strcmp("/data", parts[i]) == 0)
					int_data_size = p->GetSizeBackup();
			}

			if(type == MROM_SWAP_WITH_SECONDARY || type == MROM_SWAP_COPY_SECONDARY)
				sec_data_size = du.Get_Folder_Size(MultiROM::getRomsPath() + swap_rom + "/data");

			switch(type)
			{
				case MROM_SWAP_WITH_SECONDARY:
					needed = int_size + sec_data_size;
					break;
				case MROM_SWAP_COPY_SECONDARY:
					if(sec_data_size > int_data_size)
						needed = sec_data_size - int_data_size + 50*1024*1024;
					break;
				case MROM_SWAP_COPY_INTERNAL:
				case MROM_SWAP_MOVE_INTERNAL:
					needed = int_size;
					break;
			}

			needed >>= 20; // divide by (1024*1024) to MB
			free >>= 20;
			DataManager::SetValue("tw_multirom_swap_needed", needed);
			DataManager::SetValue("tw_multirom_swap_free", free);

			LOGINFO("multirom_swap_calc_space: needed: %llu MB, free: %llu MB\n", needed, free);

			operation_end(0, simulate);

			if(needed >= free)
			{
				DataManager::SetValue("tw_multirom_swap_calculating", 0);
				gui_changeOverlay("multirom_swap_space_info");
			}
			else
			{
				sleep(1); // give the user chance to read the overlay
				gui_changeOverlay("");
				gui_changePage("action_page");
			}
			return 0;
		}

		if(function == "multirom_execute_swap")
		{
			operation_start("SwapROMs");

			int res = 1;
			int type = DataManager::GetIntValue("tw_multirom_swap_type");
			std::string int_target = DataManager::GetStrValue("tw_multirom_swap_internal_name");

			switch(type)
			{
				case MROM_SWAP_WITH_SECONDARY:
				{
					std::string src_rom = DataManager::GetStrValue("tw_multirom_swap_rom");

					if(!MultiROM::copyInternal(int_target))
						break;

					if(!MultiROM::wipeInternal())
						break;

					if(!MultiROM::copySecondaryToInternal(src_rom))
						break;

					if(!MultiROM::erase(src_rom))
						break;

					res = 0;
					break;
				}
				case MROM_SWAP_COPY_SECONDARY:
				{
					std::string src_rom = DataManager::GetStrValue("tw_multirom_swap_rom");

					if(!MultiROM::wipeInternal())
						break;

					if(!MultiROM::copySecondaryToInternal(src_rom))
						break;

					res = 0;
					break;
				}
				case MROM_SWAP_COPY_INTERNAL:
					if(MultiROM::copyInternal(int_target))
						res = 0;
					break;
				case MROM_SWAP_MOVE_INTERNAL:
				{
					if(!MultiROM::copyInternal(int_target))
						break;

					if(!MultiROM::wipeInternal())
						break;

					res = 0;
					break;
				}
			}

			PartitionManager.Update_System_Details();

			operation_end(res, simulate);
			return 0;
		}

		if(function == "multirom_set_fw")
		{
			operation_start("CopyFW");

			std::string src = DataManager::GetStrValue("tw_filename");
			std::string dst = MultiROM::getRomsPath() + DataManager::GetStrValue("tw_multirom_rom_name") + "/firmware.img";

			gui_print("Setting ROM's radio.img to %s", src.c_str());
			int res = TWFunc::copy_file(src, dst, 0755) == 0 ? 0 : 1;

			DataManager::SetValue("tw_multirom_has_fw_image", int(access(dst.c_str(), F_OK) >= 0));

			operation_end(res, simulate);
			return 0;
		}

		if(function == "multirom_remove_fw")
		{
			operation_start("RemoveFW");

			gui_print("Removing ROM's radio.img...");
			std::string dst = MultiROM::getRomsPath() + DataManager::GetStrValue("tw_multirom_rom_name") + "/firmware.img";
			int res = remove(dst.c_str()) >= 0 ? 0 : 1;
			DataManager::SetValue("tw_multirom_has_fw_image", int(access(dst.c_str(), F_OK) >= 0));

			operation_end(res, simulate);
			return 0;
		}

		if(function == "system-image-upgrader")
		{
			operation_start("system-image-upgrader");

			int res = 0;

			if(TWFunc::Path_Exists(UBUNTU_COMMAND_FILE))
			{
				gui_print("\n");
				res = TWFunc::Exec_Cmd_Show_Output("system-image-upgrader "UBUNTU_COMMAND_FILE);
				gui_print("\n");

				if(res != 0)
				{
					gui_print("system-image-upgrader failed\n");
					res = 1;
				}
				DataManager::SetValue("system-image-upgrader-res", res);
			} else
				gui_print("Could not find system-image-upgrader command file: "UBUNTU_COMMAND_FILE"\n");

			DataManager::SetValue("tw_page_done", 1);
			operation_end(res, simulate);
			return 0;
		}

		if (function == "fileexists")
		{
			struct stat st;
			string newpath = arg + "/.";

			operation_start("FileExists");
			if (stat(arg.c_str(), &st) == 0 || stat(newpath.c_str(), &st) == 0)
				operation_end(0, simulate);
			else
				operation_end(1, simulate);
			return 0;
		}

		if (function == "flash")
		{
			int i, ret_val = 0, wipe_cache = 0;

			for (i=0; i<zip_queue_index; i++) {
				operation_start("Flashing");
				DataManager::SetValue("tw_filename", zip_queue[i]);
				DataManager::SetValue(TW_ZIP_INDEX, (i + 1));

				TWFunc::SetPerformanceMode(true);
				ret_val = flash_zip(zip_queue[i], arg, simulate, &wipe_cache);
				TWFunc::SetPerformanceMode(false);
				if (ret_val != 0) {
					gui_print("Error flashing zip '%s'\n", zip_queue[i].c_str());
					i = 10; // Error flashing zip - exit queue
					ret_val = 1;
				}
			}
			zip_queue_index = 0;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);

			if (wipe_cache)
				PartitionManager.Wipe_By_Path("/cache");

			if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) {
				operation_start("ReinjectTWRP");
				gui_print("Injecting TWRP into boot image...\n");
				if (simulate) {
					simulate_progress_bar();
				} else {
					TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
					if (Boot == NULL || Boot->Current_File_System != "emmc")
						TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash");
					else {
						string injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd=" + Boot->Actual_Block_Device;
						TWFunc::Exec_Cmd(injectcmd);
					}
					gui_print("TWRP injection complete.\n");
				}
			}

			if(DataManager::GetIntValue(TW_AUTO_INJECT_MROM) == 1 && MultiROM::folderExists())
			{
				gui_print("Injecting boot.img with MultiROM...\n");
				MultiROM::injectBoot(MultiROM::getBootDev(), true);
			}

			PartitionManager.Update_System_Details();
			operation_end(ret_val, simulate);
			return 0;
		}
		if (function == "wipe")
		{
			operation_start("Format");
			DataManager::SetValue("tw_partition", arg);

			int ret_val = false;

			if (simulate) {
				simulate_progress_bar();
			} else {
				if (arg == "data")
					ret_val = PartitionManager.Factory_Reset();
				else if (arg == "battery")
					ret_val = PartitionManager.Wipe_Battery_Stats();
				else if (arg == "rotate")
					ret_val = PartitionManager.Wipe_Rotate_Data();
				else if (arg == "dalvik")
					ret_val = PartitionManager.Wipe_Dalvik_Cache();
				else if (arg == "DATAMEDIA") {
					ret_val = PartitionManager.Format_Data();
				} else if (arg == "INTERNAL") {
					int has_datamedia, dual_storage;

					DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
					if (has_datamedia) {
						ret_val = PartitionManager.Wipe_Media_From_Data();
					} else {
						ret_val = PartitionManager.Wipe_By_Path(DataManager::GetSettingsStoragePath());
					}
				} else if (arg == "EXTERNAL") {
					string External_Path;

					DataManager::GetValue(TW_EXTERNAL_PATH, External_Path);
					ret_val = PartitionManager.Wipe_By_Path(External_Path);
				} else if (arg == "ANDROIDSECURE") {
					ret_val = PartitionManager.Wipe_Android_Secure();
				} else if (arg == "LIST") {
					string Wipe_List, wipe_path;
					bool skip = false;
					ret_val = true;
					TWPartition* wipe_part = NULL;

					DataManager::GetValue("tw_wipe_list", Wipe_List);
					LOGINFO("wipe list '%s'\n", Wipe_List.c_str());
					if (!Wipe_List.empty()) {
						size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
						while (end_pos != string::npos && start_pos < Wipe_List.size()) {
							wipe_path = Wipe_List.substr(start_pos, end_pos - start_pos);
							LOGINFO("wipe_path '%s'\n", wipe_path.c_str());
							if (wipe_path == "/and-sec") {
								if (!PartitionManager.Wipe_Android_Secure()) {
									LOGERR("Unable to wipe android secure\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							} else if (wipe_path == "DALVIK") {
								if (!PartitionManager.Wipe_Dalvik_Cache()) {
									LOGERR("Failed to wipe dalvik\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							} else if (wipe_path == "INTERNAL") {
								if (!PartitionManager.Wipe_Media_From_Data()) {
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							}
							if (!skip) {
								if (!PartitionManager.Wipe_By_Path(wipe_path)) {
									LOGERR("Unable to wipe '%s'\n", wipe_path.c_str());
									ret_val = false;
									break;
								} else if (wipe_path == DataManager::GetSettingsStoragePath()) {
									arg = wipe_path;
								}
							} else {
								skip = false;
							}
							start_pos = end_pos + 1;
							end_pos = Wipe_List.find(";", start_pos);
						}
					}
				} else
					ret_val = PartitionManager.Wipe_By_Path(arg);
#ifdef TW_OEM_BUILD
				if (arg == DataManager::GetSettingsStoragePath()) {
					// If we wiped the settings storage path, recreate the TWRP folder and dump the settings
					string Storage_Path = DataManager::GetSettingsStoragePath();

					if (PartitionManager.Mount_By_Path(Storage_Path, true)) {
						LOGINFO("Making TWRP folder and saving settings.\n");
						Storage_Path += "/TWRP";
						mkdir(Storage_Path.c_str(), 0777);
						DataManager::Flush();
					} else {
						LOGERR("Unable to recreate TWRP folder and save settings.\n");
					}
				}
#endif
			}
			PartitionManager.Update_System_Details();
			if (ret_val)
				ret_val = 0; // 0 is success
			else
				ret_val = 1; // 1 is failure
			operation_end(ret_val, simulate);
			return 0;
		}
		if (function == "refreshsizes")
		{
			operation_start("Refreshing Sizes");
			if (simulate) {
				simulate_progress_bar();
			} else
				PartitionManager.Update_System_Details();
			operation_end(0, simulate);
			return 0;
		}
		if (function == "nandroid")
		{
			operation_start("Nandroid");
			int ret = 0;

			if (simulate) {
				DataManager::SetValue("tw_partition", "Simulation");
				simulate_progress_bar();
			} else {
				if (arg == "backup") {
					string Backup_Name;
					DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
					if (Backup_Name == "(Auto Generate)" || Backup_Name == "(Current Date)" || Backup_Name == "0" || Backup_Name == "(" || PartitionManager.Check_Backup_Name(true) == 0) {
						ret = PartitionManager.Run_Backup();
					}
					else {
						operation_end(1, simulate);
						return -1;

					}
					DataManager::SetValue(TW_BACKUP_NAME, "(Auto Generate)");
				} else if (arg == "restore") {
					string Restore_Name;
					DataManager::GetValue("tw_restore", Restore_Name);
					ret = PartitionManager.Run_Restore(Restore_Name);
				} else {
					operation_end(1, simulate);
					return -1;
				}
			}
			DataManager::SetValue("tw_encrypt_backup", 0);
			if (ret == false)
				ret = 1; // 1 for failure
			else
				ret = 0; // 0 for success
			operation_end(ret, simulate);
			return 0;
		}
		if (function == "fixpermissions")
		{
			operation_start("Fix Permissions");
			LOGINFO("fix permissions started!\n");
			if (simulate) {
				simulate_progress_bar();
			} else {
				int op_status = PartitionManager.Fix_Permissions();
				if (op_status != 0)
					op_status = 1; // failure
				operation_end(op_status, simulate);
			}
			return 0;
		}
		if (function == "dd")
		{
			operation_start("imaging");

			if (simulate) {
				simulate_progress_bar();
			} else {
				string cmd = "dd " + arg;
				TWFunc::Exec_Cmd(cmd);
			}
			operation_end(0, simulate);
			return 0;
		}
		if (function == "partitionsd")
		{
			operation_start("Partition SD Card");
			int ret_val = 0;

			if (simulate) {
				simulate_progress_bar();
			} else {
				int allow_partition;
				DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
				if (allow_partition == 0) {
					gui_print("This device does not have a real SD Card!\nAborting!\n");
				} else {
					if (!PartitionManager.Partition_SDCard())
						ret_val = 1; // failed
				}
			}
			operation_end(ret_val, simulate);
			return 0;
		}
		if (function == "installhtcdumlock")
		{
			operation_start("Install HTC Dumlock");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::install_htc_dumlock();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockrestoreboot")
		{
			operation_start("HTC Dumlock Restore Boot");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::htc_dumlock_restore_original_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockreflashrecovery")
		{
			operation_start("HTC Dumlock Reflash Recovery");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::htc_dumlock_reflash_recovery_to_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "cmd")
		{
			int op_status = 0;

			operation_start("Command");
			LOGINFO("Running command: '%s'\n", arg.c_str());
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = TWFunc::Exec_Cmd(arg);
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "terminalcommand")
		{
			int op_status = 0;
			string cmdpath, command;

			DataManager::GetValue("tw_terminal_location", cmdpath);
			operation_start("CommandOutput");
			gui_print("%s # %s\n", cmdpath.c_str(), arg.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(op_status, simulate);
			} else {
				command = "cd \"" + cmdpath + "\" && " + arg + " 2>&1";;
				LOGINFO("Actual command is: '%s'\n", command.c_str());
				DataManager::SetValue("tw_terminal_command_thread", command);
				DataManager::SetValue("tw_terminal_state", 1);
				DataManager::SetValue("tw_background_thread_running", 1);
				op_status = pthread_create(&terminal_command, NULL, command_thread, NULL);
				if (op_status != 0) {
					LOGERR("Error starting terminal command thread, %i.\n", op_status);
					DataManager::SetValue("tw_terminal_state", 0);
					DataManager::SetValue("tw_background_thread_running", 0);
					operation_end(1, simulate);
				}
			}
			return 0;
		}
		if (function == "killterminal")
		{
			int op_status = 0;

			LOGINFO("Sending kill command...\n");
			operation_start("KillCommand");
			DataManager::SetValue("tw_operation_status", 0);
			DataManager::SetValue("tw_operation_state", 1);
			DataManager::SetValue("tw_terminal_state", 0);
			DataManager::SetValue("tw_background_thread_running", 0);
			DataManager::SetValue(TW_ACTION_BUSY, 0);
			return 0;
		}
		if (function == "reinjecttwrp")
		{
			int op_status = 0;
			operation_start("ReinjectTWRP");
			gui_print("Injecting TWRP into boot image...\n");
			if (simulate) {
				simulate_progress_bar();
			} else {
				TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash");
				gui_print("TWRP injection complete.\n");
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "checkbackupname")
		{
			int op_status = 0;

			operation_start("CheckBackupName");
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = PartitionManager.Check_Backup_Name(true);
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "decrypt")
		{
			int op_status = 0;

			operation_start("Decrypt");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string Password;
				DataManager::GetValue("tw_crypto_password", Password);
				op_status = PartitionManager.Decrypt_Device(Password);
				if (op_status != 0)
					op_status = 1;
				else {
					int load_theme = 1;

					DataManager::SetValue(TW_IS_ENCRYPTED, 0);

					if (load_theme) {
						int has_datamedia;

						// Check for a custom theme and load it if exists
						DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
						if (has_datamedia != 0) {
							struct stat st;
							int check = 0;
							std::string theme_path;

							theme_path = DataManager::GetSettingsStoragePath();
							if (PartitionManager.Mount_By_Path(theme_path.c_str(), 1) < 0) {
								LOGERR("Unable to mount %s during reload function startup.\n", theme_path.c_str());
								check = 1;
							}

							theme_path += "/TWRP/theme/ui.zip";
							if (check == 0 && stat(theme_path.c_str(), &st) == 0) {
								if (PageManager::ReloadPackage("TWRP", theme_path) != 0)
								{
									// Loading the custom theme failed - try loading the stock theme
									LOGINFO("Attempting to reload stock theme...\n");
									if (PageManager::ReloadPackage("TWRP", "/res/ui.xml"))
									{
										LOGERR("Failed to load base packages.\n");
									}
								}
							}
						}
					}
				}
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "adbsideload")
		{
			int ret = 0;

			operation_start("Sideload");
			if (simulate) {
				simulate_progress_bar();
			} else {
				int wipe_cache = 0;
				int wipe_dalvik = 0;
				string Sideload_File;

				if (!PartitionManager.Mount_Current_Storage(false)) {
					gui_print("Using RAM for sideload storage.\n");
					Sideload_File = "/tmp/sideload.zip";
				} else {
					Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
				}
				if (TWFunc::Path_Exists(Sideload_File)) {
					unlink(Sideload_File.c_str());
				}
				gui_print("Starting ADB sideload feature...\n");
				DataManager::GetValue("tw_wipe_dalvik", wipe_dalvik);
				ret = apply_from_adb(Sideload_File.c_str());
				DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui now that the zip install is going to start
				if (ret != 0) {
					ret = 1; // failure
				} else if (TWinstall_zip(Sideload_File.c_str(), &wipe_cache) == 0) {
					if (wipe_cache || DataManager::GetIntValue("tw_wipe_cache"))
						PartitionManager.Wipe_By_Path("/cache");
					if (wipe_dalvik)
						PartitionManager.Wipe_Dalvik_Cache();
				} else {
					ret = 1; // failure
				}
				if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) {
					operation_start("ReinjectTWRP");
					gui_print("Injecting TWRP into boot image...\n");
					if (simulate) {
						simulate_progress_bar();
					} else {
						TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
						if (Boot == NULL || Boot->Current_File_System != "emmc")
							TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash");
						else {
							string injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd=" + Boot->Actual_Block_Device;
							TWFunc::Exec_Cmd(injectcmd);
						}
						gui_print("TWRP injection complete.\n");
					}
				}
				if(DataManager::GetIntValue(TW_AUTO_INJECT_MROM) == 1 && MultiROM::folderExists())
				{
					gui_print("Injecting boot.img with MultiROM...\n");
					MultiROM::injectBoot(MultiROM::getBootDev(), true);
				}
			}
			operation_end(ret, simulate);
			return 0;
		}
		if (function == "adbsideloadcancel")
		{
			int child_pid;
			char child_prop[PROPERTY_VALUE_MAX];
			string Sideload_File;
			Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
			unlink(Sideload_File.c_str());
			property_get("tw_child_pid", child_prop, "error");
			if (strcmp(child_prop, "error") == 0) {
				LOGERR("Unable to get child ID from prop\n");
				return 0;
			}
			child_pid = atoi(child_prop);
			gui_print("Cancelling ADB sideload...\n");
			kill(child_pid, SIGTERM);
			DataManager::SetValue("tw_page_done", "1"); // For OpenRecoveryScript support
			return 0;
		}
		if (function == "openrecoveryscript") {
			operation_start("OpenRecoveryScript");
			if (simulate) {
				simulate_progress_bar();
			} else {
				// Check for the SCRIPT_FILE_TMP first as these are AOSP recovery commands
				// that we converted to ORS commands during boot in recovery.cpp.
				// Run those first.
				int reboot = 0;
				if (TWFunc::Path_Exists(SCRIPT_FILE_TMP)) {
					gui_print("Processing AOSP recovery commands...\n");
					if (OpenRecoveryScript::run_script_file() == 0) {
						reboot = 1;
					}
				}
				// Check for the ORS file in /cache and attempt to run those commands.
				if (OpenRecoveryScript::check_for_script_file()) {
					gui_print("Processing OpenRecoveryScript file...\n");
					if (OpenRecoveryScript::run_script_file() == 0) {
						reboot = 1;
					}
				}
				if (reboot && DataManager::GetIntValue(TW_ORS_IS_SECONDARY_ROM) != 1) {
					usleep(2000000); // Sleep for 2 seconds before rebooting
					TWFunc::tw_reboot(rb_system);
				} else {
					DataManager::SetValue("tw_page_done", 1);
				}
			}
			return 0;
		}
		if (function == "installsu")
		{
			int op_status = 0;

			operation_start("Install SuperSU");
			if (simulate) {
				simulate_progress_bar();
			} else {
				if (!TWFunc::Install_SuperSU())
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "fixsu")
		{
			int op_status = 0;

			operation_start("Fixing Superuser Permissions");
			if (simulate) {
				simulate_progress_bar();
			} else {
				LOGERR("Fixing su permissions was deprecated from TWRP.\n");
				LOGERR("4.3+ ROMs with SELinux will always lose su perms.\n");
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "decrypt_backup")
		{
			int op_status = 0;

			operation_start("Try Restore Decrypt");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string Restore_Path, Filename, Password;
				DataManager::GetValue("tw_restore", Restore_Path);
				Restore_Path += "/";
				DataManager::GetValue("tw_restore_password", Password);
				TWFunc::SetPerformanceMode(true);
				if (TWFunc::Try_Decrypting_Backup(Restore_Path, Password))
					op_status = 0; // success
				else
					op_status = 1; // fail
				TWFunc::SetPerformanceMode(false);
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "repair")
		{
			int op_status = 0;

			operation_start("Repair Partition");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string part_path;
				DataManager::GetValue("tw_partition_mount_point", part_path);
				if (PartitionManager.Repair_By_Path(part_path, true)) {
					op_status = 0; // success
				} else {
					LOGERR("Error repairing file system.\n");
					op_status = 1; // fail
				}
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "changefilesystem")
		{
			int op_status = 0;

			operation_start("Change File System");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string part_path, file_system;
				DataManager::GetValue("tw_partition_mount_point", part_path);
				DataManager::GetValue("tw_action_new_file_system", file_system);
				if (PartitionManager.Wipe_By_Path(part_path, file_system)) {
					op_status = 0; // success
				} else {
					LOGERR("Error changing file system.\n");
					op_status = 1; // fail
				}
			}
			PartitionManager.Update_System_Details();
			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "startmtp")
		{
			int op_status = 0;

			operation_start("Start MTP");
			if (PartitionManager.Enable_MTP())
				op_status = 0; // success
			else
				op_status = 1; // fail

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "stopmtp")
		{
			int op_status = 0;

			operation_start("Stop MTP");
			if (PartitionManager.Disable_MTP())
				op_status = 0; // success
			else
				op_status = 1; // fail

			operation_end(op_status, simulate);
			return 0;
		}
	}
	else
	{
		pthread_t t;
		pthread_create(&t, NULL, thread_start, this);
		return 0;
	}
	LOGERR("Unknown action '%s'\n", function.c_str());
	return -1;
}

int GUIAction::getKeyByName(std::string key)
{
	if (key == "home")			return KEY_HOME;
	else if (key == "menu")		return KEY_MENU;
	else if (key == "back")	 	return KEY_BACK;
	else if (key == "search")	return KEY_SEARCH;
	else if (key == "voldown")	return KEY_VOLUMEDOWN;
	else if (key == "volup")	return KEY_VOLUMEUP;
	else if (key == "power") {
		int ret_val;
		DataManager::GetValue(TW_POWER_BUTTON, ret_val);
		if (!ret_val)
			return KEY_POWER;
		else
			return ret_val;
	}

	return atol(key.c_str());
}

void* GUIAction::command_thread(void *cookie)
{
	string command;
	FILE* fp;
	char line[512];

	DataManager::GetValue("tw_terminal_command_thread", command);
	fp = popen(command.c_str(), "r");
	if (fp == NULL) {
		LOGERR("Error opening command to run.\n");
	} else {
		int fd = fileno(fp), has_data = 0, check = 0, keep_going = -1, bytes_read = 0;
		struct timeval timeout;
		fd_set fdset;

		while(keep_going)
		{
			FD_ZERO(&fdset);
			FD_SET(fd, &fdset);
			timeout.tv_sec = 0;
			timeout.tv_usec = 400000;
			has_data = select(fd+1, &fdset, NULL, NULL, &timeout);
			if (has_data == 0) {
				// Timeout reached
				DataManager::GetValue("tw_terminal_state", check);
				if (check == 0) {
					keep_going = 0;
				}
			} else if (has_data < 0) {
				// End of execution
				keep_going = 0;
			} else {
				// Try to read output
				memset(line, 0, sizeof(line));
				bytes_read = read(fd, line, sizeof(line));
				if (bytes_read > 0)
					gui_print("%s", line); // Display output
				else
					keep_going = 0; // Done executing
			}
		}
		fclose(fp);
	}
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue("tw_terminal_state", 0);
	DataManager::SetValue("tw_background_thread_running", 0);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	return NULL;
}
