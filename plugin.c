/*
    _                _      _  _____  ____   __  __  ____        _____
   / \    _ __  ___ | |__  (_)|_   _|/ ___| |  \/  || __ )   ___|_   _|
  / _ \  | '__|/ __|| '_ \ | |  | |  \___ \ | |\/| ||  _ \  / _ \ | |
 / ___ \ | |  | (__ | | | || |  | |   ___) || |  | || |_) || (_) || |
/_/   \_\|_|   \___||_| |_||_|  |_|  |____/ |_|  |_||____/  \___/ |_|

Copyright 2015 Łukasz "JustArchi" Domeradzki
Contact: JustArchi@JustArchi.net

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

//#define ARCHI_DEBUG

#define BRANCH_PREDICTION


#define _GNU_SOURCE

#ifdef _WIN32
#pragma warning (disable : 4100)  /* Disable Unreferenced parameter warning */
#include <Windows.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "public_definitions.h"
#include "public_rare_definitions.h"
#include "public_errors.h"
#include "public_errors_rare.h"
#include "ts3_functions.h"
#include "plugin.h"

static struct TS3Functions ts3Functions;

#ifdef _WIN32
#define _strcpy(dest, destSize, src) strcpy_s(dest, destSize, src)
#define snprintf sprintf_s
#else
#define _strcpy(dest, destSize, src) { strncpy(dest, src, destSize-1); (dest)[destSize-1] = '\0'; }
#endif

#define PLUGIN_API_VERSION 20

#define PATH_BUFSIZE 512
#define COMMAND_BUFSIZE 128
#define INFODATA_BUFSIZE 128
#define SERVERINFO_BUFSIZE 256
#define CHANNELINFO_BUFSIZE 512
#define RETURNCODE_BUFSIZE 128

#ifdef BRANCH_PREDICTION
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#else
#define likely(x)       x
#define unlikely(x)     x
#endif

//static char* pluginID = NULL;

#ifdef _WIN32
/* Helper function to convert wchar_T to Utf-8 encoded strings on Windows */
static int wcharToUtf8(const wchar_t* str, char** result) {
	int outlen = WideCharToMultiByte(CP_UTF8, 0, str, -1, 0, 0, 0, 0);
	*result = (char*)malloc(outlen);
	if(WideCharToMultiByte(CP_UTF8, 0, str, -1, *result, outlen, 0, 0) == 0) {
		*result = NULL;
		return -1;
	}
	return 0;
}
#endif

/*********************************** ArchiTSMBot functions ************************************/

static const char* botNickname = "ArchiTSMBot"; // Can be any valid nickname used in TS3
static const char* musicPath = "/home/ts3mb/music/"; // Absolute path to music folder, with trailing slash
static const char* rootGroup = "90521"; // Server group (ID) that has full (root) access to all commands
static const char* favWebPath = "http://radio.JustArchi.net/favs/";

// Don't change things below
static uint64 myServerConnectionHandlerID = 0;
static anyID myChannelID = -1;
static anyID myID = -1;
//static anyID toPokeID = -1;

static bool silence = false;
static bool requiresNickCorrection = true;
static bool notifyIsWorking = false;
//static bool pokeIsWorking = false;

static char botPath[PATH_BUFSIZE];
static char themeFile[PATH_BUFSIZE];
static char favPath[PATH_BUFSIZE];

//static pthread_mutex_t notifyThreadMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t notifyThread = 0;
//static pthread_t pokeThread = 0;

typedef enum {ALL, RANDOM, LAST} favPlayType;

static inline bool randomBool() {
	return rand() % 2 == 1;
}

static void toLower(char* string) {
	for (unsigned int i = 0; string[i]; ++i) {
		string[i] = tolower(string[i]);
	}
}

static void logToConsole(const char* message) {
	if (unlikely(ts3Functions.logMessage(message, LogLevel_DEBUG, "ArchiTSMBot", 0) != ERROR_ok)) {
		printf("%s\n", message);
	}
}

static void logErrorToConsole(const char* message) {
	if (unlikely(ts3Functions.logMessage(message, LogLevel_ERROR, "ArchiTSMBot", 0) != ERROR_ok)) {
		fprintf(stderr, "%s\n", message);
	}
}

static void sendErrorToChannel(const char* rawMessage) {
	if (likely(myChannelID != -1 && myServerConnectionHandlerID != 0)) {
		char message[14 + strlen(rawMessage) + 12 + 1];
		snprintf(message, sizeof(message), "%s%s%s", "[b][color=red]", rawMessage, "[/color][/b]");
		if (unlikely(ts3Functions.requestSendChannelTextMsg(myServerConnectionHandlerID, message, myChannelID, NULL) != ERROR_ok)) {
			logErrorToConsole(rawMessage);
		}
	} else {
		logErrorToConsole(rawMessage);
	}
}

#ifdef ARCHI_DEBUG
void sendErrorToChannel_int(const int rawMessage) {
	char message[10 + 1];
	snprintf(message, sizeof(message), "%d", rawMessage);
	sendErrorToChannel(message);
}
void sendErrorToChannel_unsigned_int(const unsigned int rawMessage) {
	char message[10 + 1];
	snprintf(message, sizeof(message), "%ud", rawMessage);
	sendErrorToChannel(message);
}
#endif

static void sendMessageToChannel(const char* rawMessage) {
	char message[17 + strlen(rawMessage) + 12 + 1];
	snprintf(message, sizeof(message), "%s%s%s", "[b][color=purple]", rawMessage, "[/color][/b]");
	if (unlikely(ts3Functions.requestSendChannelTextMsg(myServerConnectionHandlerID, message, myChannelID, NULL) != ERROR_ok)) {
		logToConsole(rawMessage); // In unformatted form
	}
}

static void sendMessageToChannel_2(const char* rawMessage1, const char* rawMessage2) {
	char message[strlen(rawMessage1) + strlen(rawMessage2) + 1];
	snprintf(message, sizeof(message), "%s%s", rawMessage1, rawMessage2);
	sendMessageToChannel(message);
}

/*static void removeChar(char* str, const char garbage) {
	char* src, *dst;
	for (src = dst = str; *src != '\0'; ++src) {
		*dst = *src;
		if (*dst != garbage) ++dst;
	}
	*dst = '\0';
}*/

static bool executeCommandWithErrorToChannel(const char* command) {
	FILE *stream = popen(command, "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return false;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool result = true;
	while ((read = getline(&line, &len, stream)) != -1) {
		line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
		sendErrorToChannel(line);
		result = false; // We printed something, and this is error stream
	}
	pclose(stream);
	if (unlikely(line != NULL)) {
		free(line);
	}
	return result;
}

static bool executeCommandWithOutputToChannel(const char* command) {
	FILE *stream = popen(command, "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return false;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	while ((read = getline(&line, &len, stream)) != -1) {
		line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
		sendMessageToChannel(line);
	}
	pclose(stream);
	if (likely(line != NULL)) {
		free(line);
	}
	return true;
}

static bool executeCommandWithOutput(const char* command, char** output) {
	FILE *stream = popen(command, "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return false;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	if (likely((read = getline(&line, &len, stream)) != -1)) {
		pclose(stream);
		line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
		*output = (char* ) malloc((read + 1) * sizeof(char));
		if (unlikely(!output)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("malloc() error");
			free(line);
			return false;
		}
		strncpy(*output, line, read);
		free(line);
	} else {
		*output = (char* ) malloc(sizeof(char));
		if (unlikely(!output)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("malloc() error");
			return false;
		}
		strncpy(*output, "", 0);
	}
	return true;
}

static void getArgWithDelimiter(char* messageSubstring, const char* message, const int whichOne, const char* delimiters) {
	char buffer[strlen(message) + 1];
	strncpy(buffer, message, sizeof(buffer));
	char* p = strtok(buffer, delimiters);
	int number = whichOne;
	if (number >= 0) {
		while (number > 0 && p != NULL) {
			p = strtok(NULL, delimiters);
			--number;
		}
		if (p != NULL) {
			strncpy(messageSubstring, p, sizeof(buffer));
		} else {
			strncpy(messageSubstring, "", sizeof(buffer));
		}
	} else {
		while (number < 0 && p != NULL) {
			p = strtok(NULL, delimiters);
			++number;
		}
		if (p != NULL) {
			strncpy(messageSubstring, p, sizeof(buffer));
			p = strtok(NULL, delimiters);
			while (p != NULL) {
				strncat(messageSubstring, delimiters, sizeof(buffer) - strlen(messageSubstring) - 1); // TODO: Maybe we can
				strncat(messageSubstring, p, sizeof(buffer) - strlen(messageSubstring) - 1); // do it better?
				p = strtok(NULL, delimiters);
			}
		} else {
			strncpy(messageSubstring, "", sizeof(buffer));
		}
	}
}

static void getArg(char* messageSubstring, const char* message, const int whichOne) {
	getArgWithDelimiter(messageSubstring, message, whichOne, " ");
}

/*
static bool clientBelongsToChannelGroup(const anyID fromID, const int targetGroupID) {
	int currentGroupID = 0;
	if (unlikely(ts3Functions.getClientVariableAsInt(myServerConnectionHandlerID, fromID, CLIENT_CHANNEL_GROUP_ID, &currentGroupID) != ERROR_ok)) {
		sendErrorToChannel("getClientVariableAsInt() error");
		return false;
	}
	return currentGroupID == targetGroupID;
}

static anyID getClientIDfromClientName(const char* targetName) {
	anyID ret = NULL;
	anyID *clients;
	if (unlikely(ts3Functions.getClientList(myServerConnectionHandlerID, &clients) != ERROR_ok)) {
		sendErrorToChannel("getClientList() error");
		return ret;
	}
	for (unsigned int i = 0; clients[i] != '\0'; ++i) {
		char* clientName;
		if (likely(ts3Functions.getClientVariableAsString(myServerConnectionHandlerID, clients[i], CLIENT_NICKNAME, &clientName)) == ERROR_ok) {
			if (strncasecmp(targetName, clientName, strlen(targetName)) == 0) {
				ret = clients[i];
				ts3Functions.freeMemory(clientName);
				break;
			}
			ts3Functions.freeMemory(clientName);
		} else {
			sendErrorToChannel("getClientVariableAsString() error");
			ts3Functions.freeMemory(clients);
			return ret;
		}
	}
	ts3Functions.freeMemory(clients);
	return ret;
}

static bool getClientNameByRegex(const char* regex, char** output) {
	anyID *clients;
	if (unlikely(ts3Functions.getClientList(myServerConnectionHandlerID, &clients) != ERROR_ok)) {
		sendErrorToChannel("getClientList() error");
		return false;
	}
	bool found = false;
	for (unsigned int i = 0; clients[i] != '\0'; ++i) {
		char* clientName;
		if (likely(ts3Functions.getClientVariableAsString(myServerConnectionHandlerID, clients[i], CLIENT_NICKNAME, &clientName)) == ERROR_ok) {
			if (strncasecmp(targetName, clientName, strlen(targetName)) == 0) {
				found = true;
				*output = (char* ) malloc((strlen(clientName) + 1) * sizeof(char));
				if (unlikely(!output)) {
					sendErrorToChannel(strerror(errno));
					sendErrorToChannel("malloc() error");
					ts3Functions.freeMemory(clientName);
					ts3Functions.freeMemory(clients);
					return false;
				}
				strncpy(*output, clientName, strlen(clientName));
				ts3Functions.freeMemory(clientName);
				break;
			}
			ts3Functions.freeMemory(clientName);
		} else {
			sendErrorToChannel("getClientVariableAsString() error");
			ts3Functions.freeMemory(clients);
			return ret;
		}
	}
	ts3Functions.freeMemory(clients);
	return found;
}
*/

static bool clientBelongsToServerGroup(const anyID fromID, const char* targetGroupID) {
	char* clientGroups = NULL;
	if (unlikely(ts3Functions.getClientVariableAsString(myServerConnectionHandlerID, fromID, CLIENT_SERVERGROUPS, &clientGroups) != ERROR_ok)) {
		sendErrorToChannel("getClientVariableAsString() error");
		return false;
	}
	char buffer[strlen(clientGroups) + 1];
	strncpy(buffer, clientGroups, sizeof(buffer));
	ts3Functions.freeMemory(clientGroups);
	char* clientGroup = strtok(buffer, ",");
	bool accessGranted = false;
	while (clientGroup != NULL) {
		if (strcmp(clientGroup, targetGroupID) == 0) {
			accessGranted = true;
			break;
		}
		clientGroup = strtok(NULL, ",");
	}
	return accessGranted;
}

static bool isAccessGranted(const anyID fromID, const char* targetGroupID) {
	if (clientBelongsToServerGroup(fromID, targetGroupID)) {
		return true;
	} else {
		sendMessageToChannel("Sorry! You're not permitted to use that command! :-(");
		return false;
	}
}


static void resetPlaylist() {
	executeCommandWithErrorToChannel("mpc clear >/dev/null");
	executeCommandWithErrorToChannel("mpc ls | mpc add >/dev/null");
	executeCommandWithOutputToChannel("mpc play 2>&1");
}

static bool isPlaylistRandom() {
	FILE *stream = popen("mpc status 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return false;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool ret = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strstr(line, "random: ")) { // If message contains substring
			if (strstr(line, "random: on")) { // If message contains substring
				ret = true;
			}
			break;
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	return ret;
}

static void addArtist(const char* regex, const bool one) {
	FILE *stream = popen("mpc -f %artist% ls 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, regex) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			//removeChar(line, '\'');
			char command[21 + read + 6 + 1];
			snprintf(command, sizeof(command), "%s%s%s", "mpc add -f %artist% \"", line, "\" 2>&1");
			if (likely(executeCommandWithErrorToChannel(command))) {
				snprintf(command, sizeof(command), "%s%s", "Added: ", line);
				sendMessageToChannel(command);
			}
			if (one) {
				break;
			}
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (found) {
		executeCommandWithOutputToChannel("mpc play 2>&1");
	} else {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void getArtist(const char* regex, const bool one) {
	FILE *stream = popen("mpc -f %artist% ls 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, regex) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			char command[7 + read + 1];
			snprintf(command, sizeof(command), "%s%s", "Found: ", line);
			sendMessageToChannel(command);
			if (one) {
				break;
			}
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (!found) {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void addFile(const char* regex, const bool one) {
	FILE *stream = popen("mpc -f %file% listall 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, regex) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			//removeChar(line, '\'');
			char command[19 + read + 6 + 1];
			snprintf(command, sizeof(command), "%s%s%s", "mpc add -f %file% \"", line, "\" 2>&1");
			if (likely(executeCommandWithErrorToChannel(command))) {
				snprintf(command, sizeof(command), "%s%s", "Added: ", line);
				sendMessageToChannel(command);
			}
			if (one) {
				break;
			}
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (found) {
		executeCommandWithOutputToChannel("mpc play 2>&1");
	} else {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void getFile(const char* regex, const bool one) {
	FILE *stream = popen("mpc -f %file% listall 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, regex) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			char command[7 + read + 1];
			snprintf(command, sizeof(command), "%s%s", "Found: ", line);
			sendMessageToChannel(command);
			if (one) {
				break;
			}
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (!found) {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void addSong(const char* regex, const bool one) {
	FILE *stream = popen("mpc listall 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, regex) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			//removeChar(line, '\'');
			char command[9 + read + 6 + 1];
			snprintf(command, sizeof(command), "%s%s%s", "mpc add \"", line, "\" 2>&1");
			if (likely(executeCommandWithErrorToChannel(command))) {
				snprintf(command, sizeof(command), "%s%s", "Added: ", line);
				sendMessageToChannel(command);
			}
			if (one) {
				break;
			}
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (found) {
		executeCommandWithOutputToChannel("mpc play 2>&1");
	} else {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void getSong(const char* regex, const bool one) {
	FILE *stream = popen("mpc listall 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, regex) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			char command[7 + read + 1];
			snprintf(command, sizeof(command), "%s%s", "Found: ", line);
			sendMessageToChannel(command);
			if (one) {
				break;
			}
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (!found) {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void playNum_unsigned_long_int(const unsigned long int number) {
	char command[9 + 10 + 5 + 1]; // Unsigned long int has no more than 10 digits -> <0, 4,294,967,295>
	snprintf(command, sizeof(command), "%s%lu%s", "mpc play ", number, " 2>&1");
	executeCommandWithOutputToChannel(command);
}

static void playNum(const char* regex) {
	const unsigned long int number = strtoul(regex, NULL, 0);
	if (number != 0) {
		playNum_unsigned_long_int(number);
	} else {
		sendMessageToChannel("Wrong number! :-(");
	}
}

static bool play(const char* format, const char* regex) {
	FILE *stream = NULL;
	if (format != NULL) {
		char command[7 + strlen(format) + 14 + 1];
		snprintf(command, sizeof(command), "%s%s%s", "mpc -f ", format, " playlist 2>&1");
		stream = popen(command, "r");
	} else {
		stream = popen("mpc playlist 2>&1", "r");
	}
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return false;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	unsigned long int playNumber = 0;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		++playNumber;
		if (strcasestr(line, regex) != NULL) {
			found = true;
			break;
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (found) {
		playNum_unsigned_long_int(playNumber);
		return true;
	} else {
		return false;
	}
}

static void playFile(const char* regex) {
	if (!play("%file%", regex)) {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void playSong(const char* regex) {
	if (!play(NULL, regex)) {
		sendMessageToChannel("Couldn't find anything! :-(");
	}
}

static void refreshFavSymlink(const char* clientName, const char* clientUID) {
	struct stat st = {0};

	char favFile[strlen(favPath) + strlen(clientUID) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, clientUID, ".txt");
	bool favFileExists = false;
	if (stat(favFile, &st) != -1) { // If favfile exists
		favFileExists = true;
	}

	char clientNameLower[strlen(clientName) + 1];
	strncpy(clientNameLower, clientName, sizeof(clientNameLower));
	toLower(clientNameLower);

	char favFileSymlink[strlen(favPath) + strlen(clientNameLower) + 4 + 1];
	snprintf(favFileSymlink, sizeof(favFileSymlink), "%s%s%s", favPath, clientNameLower, ".txt");
	char favFileSymlinkTarget[strlen(clientUID) + 4 + 1];
	snprintf(favFileSymlinkTarget, sizeof(favFileSymlinkTarget), "%s%s", clientUID, ".txt");

	if (lstat(favFileSymlink, &st) != -1) { // If symlink exists
		char favFileSymlinkTargetReal[st.st_size + 1];
		ssize_t r;
		if (likely((r = readlink(favFileSymlink, favFileSymlinkTargetReal, st.st_size + 1)) != -1)) {
			favFileSymlinkTargetReal[r] = '\0'; // readlink() does not append a null byte to buf
			if (strcmp(favFileSymlinkTarget, favFileSymlinkTargetReal) == 0) { // If symlinks are the same
				if (!favFileExists) { // If favfile doesn't exist (user removed his last fav)
					if (unlikely(remove(favFileSymlink))) { // Remove (now invalid) symlink to favfile
						sendErrorToChannel(strerror(errno));
						sendErrorToChannel("remove() error");
					}
				}
			} else { // If symlinks are different
				if (favFileExists) { // If favfile exists
					if (unlikely(remove(favFileSymlink))) { // Remove outdated symlink
						sendErrorToChannel(strerror(errno));
						sendErrorToChannel("remove() error");
					}
					if (unlikely(symlink(favFileSymlinkTarget, favFileSymlink))) { // Create new symlink to favfile
						sendErrorToChannel(strerror(errno));
						sendErrorToChannel("symlink() error");
					}
				}
			}
		} else {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("readlink() error");
			return;
		}
	} else { // If symlink doesn't exist
		if (favFileExists) { // If favfile exists
			if (unlikely(symlink(favFileSymlinkTarget, favFileSymlink))) { // Create new symlink to favfile
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("symlink() error");
			}
		}
	}
}

static void addFav(const char* fromUniqueIdentifier, const bool imSure) {
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	struct stat st = {0};
	bool firstFav = false;
	if (stat(favFile, &st) == -1) { // If doesn't exist yet
		firstFav = true;
	}
	FILE *cmdStream = popen("mpc -f %file% current 2>&1", "r");
	if (unlikely(!cmdStream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	if (likely((read = getline(&line, &len, cmdStream)) != -1)) {
		pclose(cmdStream);
		FILE *favStream = fopen(favFile, "a+");
		if (unlikely(!favStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			free(line);
			return;
		}
		char currentSong[read + 1];
		strncpy(currentSong, line, sizeof(currentSong));
		bool alreadyExists = false;
		while ((read = getline(&line, &len, favStream)) != -1) {
			if (strcmp(currentSong, line) == 0) {
				alreadyExists = true;
				break;
			}
		}
		free(line);
		if (alreadyExists) {
			fclose(favStream);
			sendMessageToChannel("You already faved this song! 8)");
		} else {
			if (!imSure) {
				if (randomBool()) {
					sendMessageToChannel("Magic crystall ball decided: Yup! 8)");
				} else {
					sendMessageToChannel("Magic crystall ball decided: Nope! 8)");
					return;
				}
			}
			fprintf(favStream, "%s", currentSong);
			fclose(favStream);
			sendMessageToChannel("Faved! 8)");
		}
		if (firstFav) {
			sendMessageToChannel("This is your first fav! 8)");
		}
	} else {
		sendErrorToChannel("getline() error");
		pclose(cmdStream);
		return;
	}
}

static void delFav(const char* fromUniqueIdentifier) {
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	struct stat st = {0};
	if (stat(favFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		FILE *cmdStream = popen("mpc -f %file% current 2>&1", "r");
		if (unlikely(!cmdStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("popen() error");
			return;
		}
		char favFileTemp[strlen(favFile) + 4 + 1];
		snprintf(favFileTemp, sizeof(favFileTemp), "%s%s", favFile, ".new");
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		bool alreadyRemoved = true;
		bool finalFileIsEmpty = true;
		if (likely((read = getline(&line, &len, cmdStream)) != -1)) {
			pclose(cmdStream);
			FILE *favStream = fopen(favFile, "r");
			if (unlikely(!favStream)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				free(line);
				return;
			}
			FILE *favStreamTemp = fopen(favFileTemp, "w");
			if (unlikely(!favStreamTemp)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				fclose(favStream);
				free(line);
				return;
			}
			char currentSong[read + 1];
			strncpy(currentSong, line, sizeof(currentSong));
			while ((read = getline(&line, &len, favStream)) != -1) {
				if (strcmp(currentSong, line) != 0) {
					finalFileIsEmpty = false;
					fprintf(favStreamTemp, "%s", line);
				} else {
					alreadyRemoved = false;
				}
			}
			fclose(favStream);
			fclose(favStreamTemp);
			free(line);
			if (alreadyRemoved) {
				if (unlikely(remove(favFileTemp))) {
					sendErrorToChannel(strerror(errno));
					sendErrorToChannel("remove() error");
				}
				sendMessageToChannel("You didn't fav this song! 8)");
			} else {
				sendMessageToChannel("Unfaved! 8)");
				if (finalFileIsEmpty) {
					sendMessageToChannel("That was your last fav! 8)");
					if (unlikely(remove(favFile))) {
						sendErrorToChannel(strerror(errno));
						sendErrorToChannel("remove() error");
					}
					if (unlikely(remove(favFileTemp))) {
						sendErrorToChannel(strerror(errno));
						sendErrorToChannel("remove() error");
					}
				} else {
					if (unlikely(rename(favFileTemp, favFile))) {
						sendErrorToChannel(strerror(errno));
						sendErrorToChannel("rename() error");
					}
				}
			}
		} else {
			sendErrorToChannel("getline() error");
			pclose(cmdStream);
			return;
		}
	} else {
		sendMessageToChannel("You don't have any favs yet! 8)");
	}
}

static void rankFav(const char* fromUniqueIdentifier, const char* position) {
	unsigned long int targetNumber = strtoul(position, NULL, 0);
	if (unlikely(targetNumber < 1)) {
		 targetNumber = 1;
	}
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	struct stat st = {0};
	if (stat(favFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		FILE *cmdStream = popen("mpc -f %file% current 2>&1", "r");
		if (unlikely(!cmdStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("popen() error");
			return;
		}
		char favFileTemp[strlen(favFile) + 4 + 1];
		snprintf(favFileTemp, sizeof(favFileTemp), "%s%s", favFile, ".new");
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		if (likely((read = getline(&line, &len, cmdStream)) != -1)) {
			pclose(cmdStream);
			FILE *favStream = fopen(favFile, "r");
			if (unlikely(!favStream)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				free(line);
				return;
			}
			FILE *favStreamTemp = fopen(favFileTemp, "w");
			if (unlikely(!favStreamTemp)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				fclose(favStream);
				free(line);
				return;
			}
			char currentSong[read + 1];
			strncpy(currentSong, line, sizeof(currentSong));
			unsigned long int favNumber = 0;
			while ((read = getline(&line, &len, favStream)) != -1) {
				if (++favNumber == targetNumber) {
					fprintf(favStreamTemp, "%s", currentSong);
				}
				if (strcmp(currentSong, line) != 0) {
					fprintf(favStreamTemp, "%s", line);
				}
			}
			fclose(favStream);
			if (favNumber < targetNumber) { // Target number is bigger than all positions, add fav on the last position
				fprintf(favStreamTemp, "%s", currentSong);
			}
			fclose(favStreamTemp);
			free(line);
			if (unlikely(rename(favFileTemp, favFile))) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("rename() error");
				return;
			}
			sendMessageToChannel("Done! 8)");
		} else {
			sendErrorToChannel("getline() error");
			pclose(cmdStream);
			return;
		}
	} else {
		sendMessageToChannel("You don't have any favs yet! 8)");
	}
}

static void fixFavs(const char* fromUniqueIdentifier) {
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	struct stat st = {0};
	if (stat(favFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		char favFileTemp[strlen(favFile) + 4 + 1];
		snprintf(favFileTemp, sizeof(favFileTemp), "%s%s", favFile, ".new");
		FILE *favStream = fopen(favFile, "r");
		if (unlikely(!favStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			return;
		}
		FILE *favStreamTemp = fopen(favFileTemp, "w");
		if (unlikely(!favStreamTemp)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			fclose(favStream);
			return;
		}
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		bool fixedSomething = false;
		while ((read = getline(&line, &len, favStream)) != -1) {
			char fileToDelete[strlen(musicPath) + read + 1];
			snprintf(fileToDelete, sizeof(fileToDelete), "%s%s", musicPath, line);
			fileToDelete[strcspn(fileToDelete, "\r\n")] = 0; // Make sure that there are no newlines
			if (stat(fileToDelete, &st) == -1) { // If file doesn't exist
				fixedSomething = true;
			} else {
				fprintf(favStreamTemp, "%s", line);
			}
		}
		fclose(favStream);
		fclose(favStreamTemp);
		if (line != NULL) {
			free(line);
		}
		if (fixedSomething) {
			if (unlikely(rename(favFileTemp, favFile))) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("rename() error");
			}
			sendMessageToChannel("Fixed! 8)");
		} else {
			if (unlikely(remove(favFileTemp))) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("remove() error");
			}
			sendMessageToChannel("Nothing to fix! 8)");
		}
	} else {
		sendMessageToChannel("You don't have any favs yet! 8)");
	}
}

static void getFav(const char* fromUniqueIdentifier) {
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	struct stat st = {0};
	if (stat(favFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		FILE *favStream = fopen(favFile, "r");
		if (unlikely(!favStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			return;
		}
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		unsigned long int counter = 0;
		while ((read = getline(&line, &len, favStream)) != -1) {
			++counter;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			sendMessageToChannel(line);
		}
		fclose(favStream);
		if (line != NULL) {
			free(line);
		}
		sendMessageToChannel("----------");
		char message[5 + 10 + 1];
		snprintf(message, sizeof(message), "%s%lu", "Sum: ", counter);
		sendMessageToChannel(message);
	} else {
		sendMessageToChannel("You don't have any favs yet! 8)");
	}
}

static void playFav(const char* fromUniqueIdentifier, const favPlayType favPlayType, const bool insert) {
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	struct stat st = {0};
	if (stat(favFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		if (favPlayType == ALL) {
			if (!insert) {
				executeCommandWithErrorToChannel("mpc clear >/dev/null");
				char command[11 + strlen(favFile) + 12 + 1];
				snprintf(command, sizeof(command), "%s%s%s", "mpc add < \'", favFile, "\' >/dev/null");
				executeCommandWithErrorToChannel(command);
				executeCommandWithOutputToChannel("mpc play 2>&1");
			} else {
				if (isPlaylistRandom()) {
					executeCommandWithErrorToChannel("mpc random off >/dev/null");
					executeCommandWithErrorToChannel("mpc shuffle >/dev/null");
				}
				char command[14 + strlen(favFile) + 6 + 1];
				snprintf(command, sizeof(command), "%s%s%s", "mpc insert < \'", favFile, "\' 2>&1");
				executeCommandWithErrorToChannel(command);
			}
		} else {
			FILE *favStream = fopen(favFile, "r");
			if (unlikely(!favStream)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				return;
			}
			char* line = NULL;
			size_t len = 0;
			ssize_t read = -1;
			unsigned int lines = 0;
			while ((read = getline(&line, &len, favStream)) != -1) {
				++lines;
			}
			fclose(favStream);

			unsigned int targetLine;
			if (favPlayType == RANDOM) {
				targetLine = rand() % lines + 1; // <1, lines>
			} else {
				targetLine = lines;
			}

			favStream = fopen(favFile, "r");
			if (unlikely(!favStream)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				if (line != NULL) {
					free(line);
				}
				return;
			}
			lines = 0;
			while ((read = getline(&line, &len, favStream)) != -1) {
				if (targetLine == ++lines) {
					line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
					if (!insert) {
						if (!play("%file%", line)) { // Try to play the file from playlist first, maybe we don't need to reset it
							executeCommandWithErrorToChannel("mpc clear >/dev/null");
							char command[19 + read + 6 + 1];
							snprintf(command, sizeof(command), "%s%s%s", "mpc -f %file% add \'", line, "\' 2>&1");
							executeCommandWithErrorToChannel(command);
							executeCommandWithOutputToChannel("mpc play 2>&1");
						}
					} else {
						if (isPlaylistRandom()) {
							executeCommandWithErrorToChannel("mpc random off >/dev/null");
							executeCommandWithErrorToChannel("mpc shuffle >/dev/null");
						}
						char command[22 + read + 12 + 1];
						snprintf(command, sizeof(command), "%s%s%s", "mpc -f %file% insert \'", line, "\' >/dev/null");
						executeCommandWithErrorToChannel(command);
						snprintf(command, sizeof(command), "%s%s", "Added: ", line);
						sendMessageToChannel(command);
					}
					break;
				}
			}
			fclose(favStream);
			if (line != NULL) {
				free(line);
			}
		}
	} else {
		sendMessageToChannel("You don't have any favs yet! 8)");
	}
}

static void zipFav(const char* fromUniqueIdentifier) {
	char favFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(favFile, sizeof(favFile), "%s%s%s", favPath, fromUniqueIdentifier, ".txt");
	char zipFile[strlen(favPath) + strlen(fromUniqueIdentifier) + 4 + 1];
	snprintf(zipFile, sizeof(zipFile), "%s%s%s", favPath, fromUniqueIdentifier, ".zip");
	struct stat st = {0};
	if (stat(favFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		if (stat(zipFile, &st) != -1) { // If zipfile exists
			if (unlikely(remove(zipFile))) { // Remove previous zipfile
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("remove() error");
				return;
			}
		}
		FILE *favStream = fopen(favFile, "r");
		if (unlikely(!favStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			return;
		}
		unsigned int mallocCount = 7 + strlen(zipFile) + 1 + 1;
		char* command = (char*) malloc(mallocCount * sizeof(char));
		if (unlikely(!command)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("malloc() error");
			fclose(favStream);
			return;
		}
		snprintf(command, mallocCount, "%s%s%s", "zip -0 ", zipFile, " ");
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		while ((read = getline(&line, &len, favStream)) != -1) {
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			mallocCount += (1 + strlen(musicPath) + read + 2) * sizeof(char);
			command = (char*) realloc(command, mallocCount);
			if (unlikely(!command)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("malloc() error");
				fclose(favStream);
				free(line);
				return;
			}
			strncat(command, "\"", 1);
			strncat(command, musicPath, strlen(musicPath));
			strncat(command, line, read);
			strncat(command, "\" ", 2);
		}
		fclose(favStream);
		free(line);
		mallocCount += 10;
		command = (char* ) realloc(command, mallocCount);
		if (unlikely(!command)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("malloc() error");
			return;
		}
		strncat(command, ">/dev/null", 10);
		sendMessageToChannel("Working...");
		if (likely(executeCommandWithErrorToChannel(command))) {
			char message[36 + strlen(favWebPath) + strlen(fromUniqueIdentifier) + 22 + 1];
			snprintf(message, sizeof(message), "%s%s%s%s", "Done! You can find your zip [b][url=", favWebPath , fromUniqueIdentifier, ".zip]here[/url][/b] 8)");
			sendMessageToChannel(message);
		} else {
			sendErrorToChannel("Error! :-(");
		}
		free(command);
	} else {
		sendMessageToChannel("You don't have any favs yet! 8)");
	}
}

static void addTheme(const char* theme) {
	FILE *themeStream = fopen(themeFile, "a+");
	if (unlikely(!themeStream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("fopen() error");
		return;
	}
	bool alreadyExists = false;
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	while ((read = getline(&line, &len, themeStream)) != -1) {
		if (strncmp(theme, line, strlen(theme)) == 0) {
			alreadyExists = true;
			break;
		}
	}
	if (likely(line != NULL)) {
		free(line);
	}
	if (alreadyExists) {
		fclose(themeStream);
		sendMessageToChannel("This theme exists already! 8)");
	} else {
		fprintf(themeStream, "%s\n", theme);
		fclose(themeStream);
		sendMessageToChannel("Theme added! 8)");
	}
}

static void getTheme(const char* theme) {
	struct stat st = {0};
	if (stat(themeFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		FILE *themeStream = fopen(themeFile, "r");
		if (unlikely(!themeStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			return;
		}
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		bool found = false;
		if (theme == NULL) {
			while ((read = getline(&line, &len, themeStream)) != -1) {
				found = true;
				line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
				sendMessageToChannel(line);
			}
		} else {
			while ((read = getline(&line, &len, themeStream)) != -1) {
				if (strcasestr(line, theme) != NULL) {
					found = true;
					line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
					sendMessageToChannel(line);
				}
			}
		}
		fclose(themeStream);
		free(line);
		if (!found) {
			sendMessageToChannel("Couldn't find anything! :-(");
		}
	} else {
		sendMessageToChannel("No themes added yet! 8)");
	}
}

static void setTheme(const char* theme, const bool fixed) {
	if (!fixed) {
		struct stat st = {0};
		if (stat(themeFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
			FILE *themeStream = fopen(themeFile, "r");
			if (unlikely(!themeStream)) {
				sendErrorToChannel(strerror(errno));
				sendErrorToChannel("fopen() error");
				return;
			}
			char* line = NULL;
			size_t len = 0;
			ssize_t read = -1;
			bool found = false;
			while ((read = getline(&line, &len, themeStream)) != -1) {
				if (strcasestr(line, theme) != NULL) {
					sendMessageToChannel("Tagging...");
					found = true;
					line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
					char* output = NULL;
					if (likely(executeCommandWithOutput("mpc current -f %file% 2>&1", &output))) {
						char command[19 + read + 3 + strlen(musicPath) + strlen(output) + 6 + 1];
						snprintf(command, sizeof(command), "%s%s%s%s%s%s", "id3v2 -2 -c \"Theme:", line, "\" \"", musicPath, output, "\" 2>&1");
						free(output);
						if (likely(executeCommandWithErrorToChannel(command))) {
							executeCommandWithErrorToChannel("mpc update --wait >/dev/null");
							char message[15 + read + 1];
							snprintf(message, sizeof(message), "%s%s", "Classified as: ", line);
							sendMessageToChannel(message);
						}
					} else {
						sendErrorToChannel("executeCommandWithOutput() error");
					}
					break;
				}
			}
			fclose(themeStream);
			free(line);
			if (!found) {
				sendMessageToChannel("Couldn't find anything! :-(");
			}
		} else {
			sendMessageToChannel("No themes added yet! 8)");
		}
	} else {
		char* output = NULL;
		if (likely(executeCommandWithOutput("mpc current -f %file% 2>&1", &output))) {
			char command[19 + strlen(theme) + 3 + strlen(musicPath) + strlen(output) + 6 + 1];
			snprintf(command, sizeof(command), "%s%s%s%s%s%s", "id3v2 -2 -c \"Theme:", theme, "\" \"", musicPath, output, "\" 2>&1");
			free(output);
			if (likely(executeCommandWithErrorToChannel(command))) {
				executeCommandWithErrorToChannel("mpc update --wait >/dev/null");
				char message[15 + strlen(theme) + 1];
				snprintf(message, sizeof(message), "%s%s", "Classified as: ", theme);
				sendMessageToChannel(message);
			}
		} else {
			sendErrorToChannel("executeCommandWithOutput() error");
		}
	}
}

static void playTheme(const char* regex) {
	struct stat st = {0};
	if (stat(themeFile, &st) != -1 && st.st_size != 0) { // If file exists and is non-empty
		FILE *themeStream = fopen(themeFile, "r");
		if (unlikely(!themeStream)) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("fopen() error");
			return;
		}
		char* line = NULL;
		size_t len = 0;
		ssize_t read = -1;
		bool found = false;
		while ((read = getline(&line, &len, themeStream)) != -1) {
			if (strcasestr(line, regex) != NULL) {
				char foundTheme[read + 1];
				strncpy(foundTheme, line, sizeof(foundTheme));
				foundTheme[strcspn(foundTheme, "\r\n")] = 0; // Make sure that there are no newlines
				FILE *stream = popen("mpc -f %comment%:%file% listall 2>&1", "r");
				if (unlikely(!stream)) {
					sendErrorToChannel(strerror(errno));
					sendErrorToChannel("popen() error");
					fclose(themeStream);
					free(line);
					return;
				}
				executeCommandWithErrorToChannel("mpc clear >/dev/null");
				while ((read = getline(&line, &len, stream)) != -1) {
					if (strcasestr(line, foundTheme) != NULL) {
						found = true;
						char foundFile[read + 1];
						getArgWithDelimiter(foundFile, line, 1, ":");
						foundFile[strcspn(foundFile, "\r\n")] = 0; // Make sure that there are no newlines
						//removeChar(foundFile, '\'');
						char command[19 + read + 6 + 1];
						snprintf(command, sizeof(command), "%s%s%s", "mpc add -f %file% \"", foundFile, "\" 2>&1");
						if (likely(executeCommandWithErrorToChannel(command))) {
							char message[7 + strlen(foundFile) + 1];
							snprintf(message, sizeof(message), "%s%s", "Added: ", foundFile);
							sendMessageToChannel(message);
						}
					}
				}
				pclose(stream);
				break;
			}
		}
		fclose(themeStream);
		free(line);
		if (found) {
			sendMessageToChannel("---");
			executeCommandWithOutputToChannel("mpc play 2>&1");
		} else {
			sendMessageToChannel("Couldn't find anything! :-(");
			sendMessageToChannel("---");
			resetPlaylist();
		}
	} else {
		sendMessageToChannel("No themes added yet! 8)");
	}
}

static void delSong() {
	char* output = NULL;
	if (likely(executeCommandWithOutput("mpc current -f %file% 2>&1", &output))) {
		char fileToDelete[strlen(musicPath) + strlen(output) + 1];
		snprintf(fileToDelete, sizeof(fileToDelete), "%s%s", musicPath, output);
		free(output);
		executeCommandWithErrorToChannel("mpc clear >/dev/null");
		if (unlikely(remove(fileToDelete))) {
			sendErrorToChannel(strerror(errno));
			sendErrorToChannel("remove() error");
		}
		resetPlaylist();
	} else {
		sendErrorToChannel("executeCommandWithOutput() error");
	}
}

static inline void pokeID(const anyID toPoke, const char* pokeMessage) {
	if (ts3Functions.requestClientPoke(myServerConnectionHandlerID, toPoke, pokeMessage, NULL) != ERROR_ok) {
		sendErrorToChannel("requestClientPoke() error");
	}
}

static void pokeUser(const char* toPoke, const char* pokeMessage, const unsigned int howManyTimes) {
	anyID *clients;
	if (unlikely(ts3Functions.getClientList(myServerConnectionHandlerID, &clients) != ERROR_ok)) {
		sendErrorToChannel("getClientList() error");
		return;
	}
	bool found = false;
	for (unsigned int i = 0; clients[i] != '\0'; ++i) {
		char* clientName = NULL;
		if (likely(ts3Functions.getClientVariableAsString(myServerConnectionHandlerID, clients[i], CLIENT_NICKNAME, &clientName)) == ERROR_ok) {
			if (strcasecmp(toPoke, clientName) == 0) {
				found = true;
				char message[7 + strlen(clientName) + 1];
				snprintf(message, sizeof(message), "%s%s", "Poked: ", clientName);
				ts3Functions.freeMemory(clientName);
				for (unsigned int pokeNum = howManyTimes; pokeNum > 0; --pokeNum) {
					pokeID(clients[i], pokeMessage);
				}
				sendMessageToChannel(message);
				break;
			}
			ts3Functions.freeMemory(clientName);
		} else {
			sendErrorToChannel("getClientVariableAsString() error");
			ts3Functions.freeMemory(clients);
			return;
		}
	}
	ts3Functions.freeMemory(clients);
	if (!found) {
		sendMessageToChannel("Couldn't find anybody! :-(");
	}
}

static void guessSong(const char* guess) {
	FILE *stream = popen("mpc -f %artist% current 2>&1", "r");
	if (unlikely(!stream)) {
		sendErrorToChannel(strerror(errno));
		sendErrorToChannel("popen() error");
		return;
	}
	char* line = NULL;
	size_t len = 0;
	ssize_t read = -1;
	bool found = false;
	while ((read = getline(&line, &len, stream)) != -1) {
		if (strcasestr(line, guess) != NULL) {
			found = true;
			line[strcspn(line, "\r\n")] = 0; // Make sure that there are no newlines
			//removeChar(line, '\'');
			char message[19 + read + 4 + 1];
			snprintf(message, sizeof(message), "%s%s%s", "That's right! It's ", line, "! 8)");
			sendMessageToChannel(message);
			break;
		}
	}
	pclose(stream);
	if (line != NULL) {
		free(line);
	}
	if (!found) {
		sendMessageToChannel("Nope, try again! 8)");
	}
}





static bool notifyWorkerIsRunning() {
	if (notifyThread != 0) {
		if (pthread_kill(notifyThread, 0) != ESRCH) {
			return true;
		} else {
			notifyThread = 0; // So we won't try to send signal to invalid pthread next time
		}
	}
	return false;
}

static void *notifyWorker(void *args) {
	char* output = NULL;
	while (executeCommandWithOutput("mpc current --wait 2>&1", &output) && notifyIsWorking) {
		sendMessageToChannel_2("Current song: ", output);
		free(output);
		output = NULL;
	}
	if (output != NULL) {
		free(output);
	}
	return NULL;
}

/*static bool pokeWorkerIsRunning() {
	if (pokeThread != 0) {
		if (pthread_kill(pokeThread, 0) != ESRCH) {
			return true;
		} else {
			pokeThread = 0; // So we won't try to send signal to invalid pthread next time
		}
	}
	return false;
}

static void *pokeWorker(void *args) {
	while (pokeIsWorking) {
		if (pokeID(toPokeID, "")) {
			sleep(1);
		} else {
			pokeIsWorking = false;
		}
	}
	toPokeID = -1;
	return NULL;
}*/

/*********************************** Required functions ************************************/
/*
 * If any of these required functions is not implemented, TS3 will refuse to load the plugin
 */

/* Unique name identifying this plugin */
const char* ts3plugin_name() {
#ifdef _WIN32
	/* TeamSpeak expects UTF-8 encoded characters. Following demonstrates a possibility how to convert UTF-16 wchar_t into UTF-8. */
	static char* result = NULL;  /* Static variable so it's allocated only once */
	if(!result) {
		const wchar_t* name = L"ArchiTSMBot";
		if(wcharToUtf8(name, &result) == -1) {  /* Convert name into UTF-8 encoded result */
			result = "ArchiTSMBot";  /* Conversion failed, fallback here */
		}
	}
	return result;
#else
	return "ArchiTSMBot";
#endif
}

/* Plugin version */
const char* ts3plugin_version() {
	return "2.0";
}

/* Plugin API version. Must be the same as the clients API major version, else the plugin fails to load. */
int ts3plugin_apiVersion() {
	return PLUGIN_API_VERSION;
}

/* Plugin author */
const char* ts3plugin_author() {
	/* If you want to use wchar_t, see ts3plugin_name() on how to use */
	return "Łukasz \"JustArchi\" Domeradzki";
}

/* Plugin description */
const char* ts3plugin_description() {
	/* If you want to use wchar_t, see ts3plugin_name() on how to use */
	return "ArchiTSMBot";
}

/* Set TeamSpeak 3 callback functions */
void ts3plugin_setFunctionPointers(const struct TS3Functions funcs) {
	ts3Functions = funcs;
}

int ts3plugin_init() {

	ts3Functions.getPluginPath(botPath, PATH_BUFSIZE);
	if (likely((sizeof(botPath) - strlen(botPath) - 1) >= 56)) { // 19 for botPath, 5 for favPath, 28 for UID, 4 for ".txt", this is max
		struct stat st = {0};

		strncat(botPath, "architsmbot_plugin/", sizeof(botPath) - strlen(botPath) - 1);
		if (stat(botPath, &st) == -1) {
			mkdir(botPath, 0700);
		}

		snprintf(themeFile, sizeof(themeFile), "%s%s", botPath, "themes.txt");

		snprintf(favPath, sizeof(favPath), "%s%s", botPath, "favs/");
		if (stat(favPath, &st) == -1) {
			mkdir(favPath, 0700);
		}
	} else {
		sendErrorToChannel("FATAL ERROR: botPath too long, this is undefined behaviour and shouldn't happen!");
		return 1;
	}

	return 0;
}

void ts3plugin_shutdown() {
	/* Free pluginID if we registered it */
	/*if (pluginID) {
		free(pluginID);
		pluginID = NULL;
	}*/
}

/****************************** Optional functions ********************************/
/*
 * Following functions are optional, if not needed you don't need to implement them.
 */

/* Plugin command keyword. Return NULL or "" if not used. */
const char* ts3plugin_commandKeyword() {
	return "ArchiTSMBot";
}

/*
 * Plugin requests to be always automatically loaded by the TeamSpeak 3 client unless
 * the user manually disabled it in the plugin dialog.
 * This function is optional. If missing, no autoload is assumed.
 */
int ts3plugin_requestAutoload() {
	return 1;  /* 1 = request autoloaded, 0 = do not request autoload */
}

/************************** TeamSpeak callbacks ***************************/
/*
 * Following functions are optional, feel free to remove unused callbacks.
 * See the clientlib documentation for details on each function.
 */

/* Clientlib */

void ts3plugin_onConnectStatusChangeEvent(uint64 serverConnectionHandlerID, int newStatus, unsigned int errorNumber) {
	if (newStatus == STATUS_CONNECTION_ESTABLISHED) {
		// Set our serverConnectionHandlerID
		myServerConnectionHandlerID = serverConnectionHandlerID;

		// Set our ID
		if (unlikely(ts3Functions.getClientID(serverConnectionHandlerID, &myID) != ERROR_ok)) {
			sendErrorToChannel("getClientID() error");
			return;
		}

		// Set our current channel
		uint64 toID;
		if (unlikely(ts3Functions.getChannelOfClient(serverConnectionHandlerID, myID, &toID) != ERROR_ok)) {
			sendErrorToChannel("getChannelOfClient() error");
			return;
		}
		myChannelID = toID;

		// Say hello
		sendMessageToChannel("Hello! 8)");

		// Try to set channel commander for self
		if (unlikely(ts3Functions.setClientSelfVariableAsInt(serverConnectionHandlerID, CLIENT_IS_CHANNEL_COMMANDER, 1) != ERROR_ok)) {
			sendErrorToChannel("setClientSelfVariableAsInt() error");
			return;
		}
		if (unlikely(ts3Functions.flushClientSelfUpdates(serverConnectionHandlerID, NULL) != ERROR_ok)) {
			sendErrorToChannel("flushClientSelfUpdates() error");
			return;
		}

		// Check if we need to correct nickname
		char* currentNickname;
		if (unlikely(ts3Functions.getClientSelfVariableAsString(serverConnectionHandlerID, CLIENT_NICKNAME, &currentNickname) != ERROR_ok)) {
			sendErrorToChannel("getClientSelfVariableAsString() error");
			return;
		} else {
			if (strcmp(botNickname, currentNickname) == 0) {
				requiresNickCorrection = false;
			}
			ts3Functions.freeMemory(currentNickname);
		}

		// Check if we belong to rootGroup
		if (!clientBelongsToServerGroup(myID, rootGroup)) {
			sendErrorToChannel("WARNING: Bot does not belong to the rootGroup!");
		}
	}
}

//void ts3plugin_onNewChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 channelParentID) {
//}

//void ts3plugin_onNewChannelCreatedEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 channelParentID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
//}

//void ts3plugin_onDelChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
//}

//void ts3plugin_onChannelMoveEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 newChannelParentID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
//}

//void ts3plugin_onUpdateChannelEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
//}

//void ts3plugin_onUpdateChannelEditedEvent(uint64 serverConnectionHandlerID, uint64 channelID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
//}

//void ts3plugin_onUpdateClientEvent(uint64 serverConnectionHandlerID, anyID clientID, anyID invokerID, const char* invokerName, const char* invokerUniqueIdentifier) {
//}

//void ts3plugin_onClientMoveEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* moveMessage) {
//}

//void ts3plugin_onClientMoveSubscriptionEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility) {
//}

void ts3plugin_onClientMoveTimeoutEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, const char* timeoutMessage) {
	if (unlikely(requiresNickCorrection)) {
		if (unlikely(ts3Functions.setClientSelfVariableAsString(serverConnectionHandlerID, CLIENT_NICKNAME, botNickname) != ERROR_ok)) {
			sendErrorToChannel("setClientSelfVariableAsString() error");
			return;
		}
		if (unlikely(ts3Functions.flushClientSelfUpdates(serverConnectionHandlerID, NULL) != ERROR_ok)) {
			sendErrorToChannel("flushClientSelfUpdates() error");
			return;
		}
		// Continuation in ts3plugin_onClientDisplayNameChanged(), if we succeed
	}
}

//void ts3plugin_onClientMoveMovedEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID moverID, const char* moverName, const char* moverUniqueIdentifier, const char* moveMessage) {
//}

//void ts3plugin_onClientKickFromChannelEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
//}

//void ts3plugin_onClientKickFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, const char* kickMessage) {
//}

//void ts3plugin_onClientIDsEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, anyID clientID, const char* clientName) {
//}

//void ts3plugin_onClientIDsFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onServerEditedEvent(uint64 serverConnectionHandlerID, anyID editerID, const char* editerName, const char* editerUniqueIdentifier) {
//}

//void ts3plugin_onServerUpdatedEvent(uint64 serverConnectionHandlerID) {
//}

//int ts3plugin_onServerErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, const char* extraMessage) {
//	return 0;
//}

//void ts3plugin_onServerStopEvent(uint64 serverConnectionHandlerID, const char* shutdownMessage) {
//}

int ts3plugin_onTextMessageEvent(uint64 serverConnectionHandlerID, anyID targetMode, anyID toID, anyID fromID, const char* fromName, const char* fromUniqueIdentifier, const char* message, int ffIgnored) {
	if (ffIgnored) {
		return 1; /* Client will ignore the message anyways, so return value here doesn't matter */
	}

	if (targetMode == TextMessageTarget_CLIENT) {
		if (unlikely(ts3Functions.requestSendPrivateTextMsg(serverConnectionHandlerID, "Sorry, I listen only to channel messages Senpai! :-(", fromID, NULL) != ERROR_ok)) {
			sendErrorToChannel("requestSendPrivateTextMsg() error");
		}
		return 1;
	} else if (targetMode == TextMessageTarget_CHANNEL) {
		if (fromID != myID) {  /* Don't reply when source is own client */
			if (strstr(message, "!") == message) { // If message starts with specific char
				if (strcasecmp(message, "!shh") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						silence = !silence;
						sendMessageToChannel("( ͡° ͜ʖ ͡°)");
					}
				} else if (silence) {
					sendMessageToChannel("( ͡° ͜ʖ ͡°)");
				} else if (strncasecmp(message, "!addartist ", 11) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addArtist(messageSubstring, true);
					}
				} else if (strncasecmp(message, "!addartists ", 12) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addArtist(messageSubstring, false);
					}
				} else if (strncasecmp(message, "!addfile ", 9) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addFile(messageSubstring, true);
					}
				} else if (strncasecmp(message, "!addfiles ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addFile(messageSubstring, false);
					}
				} else if (strncasecmp(message, "!addsong ", 9) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addSong(messageSubstring, true);
					}
				} else if (strncasecmp(message, "!addsongs ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addSong(messageSubstring, false);
					}
				} else if (strncasecmp(message, "!addtheme ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						addTheme(messageSubstring);
					}
				} else if (strncasecmp(message, "!artist ", 8) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getArtist(messageSubstring, true);
				} else if (strcasecmp(message, "!artists") == 0) {
					executeCommandWithOutputToChannel("mpc ls 2>&1");
				} else if (strncasecmp(message, "!artists ", 9) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getArtist(messageSubstring, false);
				} else if (strcasecmp(message, "!clear") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc clear 2>&1");
					}
				} else if (strcasecmp(message, "!consume") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc consume 2>&1");
					}
#ifdef ARCHI_DEBUG
				} else if (strcasecmp(message, "!debug") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						sendErrorToChannel("Pompf");
					}
				} else if (strncasecmp(message, "!debug ", 7) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, 1);
						sendErrorToChannel(messageSubstring);
					}
#endif
				} else if (strcasecmp(message, "!fav") == 0) {
					addFav(fromUniqueIdentifier, true);
					refreshFavSymlink(fromName, fromUniqueIdentifier);
				} else if (strcasecmp(message, "!fav?") == 0) {
					addFav(fromUniqueIdentifier, false);
					refreshFavSymlink(fromName, fromUniqueIdentifier);
				} else if (strcasecmp(message, "!favs") == 0) {
					getFav(fromUniqueIdentifier);
					refreshFavSymlink(fromName, fromUniqueIdentifier);
				} else if (strncasecmp(message, "!favs ", 6) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getFav(messageSubstring);
				} else if (strcasecmp(message, "!file") == 0) {
					executeCommandWithOutputToChannel("mpc -f %file% current 2>&1");
				} else if (strncasecmp(message, "!file ", 6) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getFile(messageSubstring, true);
				} else if (strcasecmp(message, "!files") == 0) {
					executeCommandWithOutputToChannel("mpc -f %file% listall 2>&1");
				} else if (strncasecmp(message, "!files ", 7) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getFile(messageSubstring, false);
				} else if (strcasecmp(message, "!fixfavs") == 0) {
					fixFavs(fromUniqueIdentifier);
					refreshFavSymlink(fromName, fromUniqueIdentifier);
				} else if (strncasecmp(message, "!guess ", 7) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					guessSong(messageSubstring);
				} else if (strcasecmp(message, "!lastfav") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						playFav(fromUniqueIdentifier, LAST, false);
						refreshFavSymlink(fromName, fromUniqueIdentifier);
					}
				} else if (strncasecmp(message, "!lastfav ", 9) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playFav(messageSubstring, LAST, false);
					}
				} else if (strcasecmp(message, "!next") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc next 2>&1");
					}
				} else if (strcasecmp(message, "!nextfav") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						playFav(fromUniqueIdentifier, RANDOM, true);
						refreshFavSymlink(fromName, fromUniqueIdentifier);
					}
				} else if (strncasecmp(message, "!nextfav ", 9) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playFav(messageSubstring, RANDOM, true);
					}
				} else if (strcasecmp(message, "!notify") == 0) {
					if (notifyIsWorking) {
						notifyIsWorking = false;
						sendMessageToChannel("Notifier: OFF! Silence is golden! 8)");
					} else {
						notifyIsWorking = true;
						if (!notifyWorkerIsRunning()) {
							if (unlikely(pthread_create(&notifyThread, NULL, &notifyWorker, (void*) NULL))) {
								sendErrorToChannel("pthread_create() error");
								return 1;
							} else {
								pthread_detach(notifyThread);
							}
						}
						sendMessageToChannel("Notifier: ON! Title of every song will be displayed! 8)");
					}
				} else if (strcasecmp(message, "!pause") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc toggle 2>&1");
					}
				} else if (strcasecmp(message, "!play") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc play 2>&1");
					}
				} else if (strncasecmp(message, "!play ", 6) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playNum(messageSubstring);
					}
				} else if (strcasecmp(message, "!playfavs") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						playFav(fromUniqueIdentifier, ALL, false);
					}
				} else if (strncasecmp(message, "!playfavs ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playFav(messageSubstring, ALL, false);
					}
				} else if (strncasecmp(message, "!playfile ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playFile(messageSubstring);
					}
				} else if (strncasecmp(message, "!playsong ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playSong(messageSubstring);
					}
				} else if (strncasecmp(message, "!playtheme ", 11) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playTheme(messageSubstring);
					}
				} else if (strncasecmp(message, "!poke ", 6) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char toPoke[strlen(message) + 1];
						getArg(toPoke, message, 1);
						char pokeMessage[strlen(message) + 1];
						getArg(pokeMessage, message, -2);
						pokeUser(toPoke, pokeMessage, 1);
					}
/*				} else if (strcasecmp(message, "!pokespam") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						if (pokeIsWorking) {
							pokeIsWorking = false;
							sendMessageToChannel("Stopped spamming! 8)");
						}
					}*/
				} else if (strncasecmp(message, "!pokespam ", 10) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char toPoke[strlen(message) + 1];
						getArg(toPoke, message, 1);
						char pokeMessage[strlen(message) + 1];
						getArg(pokeMessage, message, -2);
						pokeUser(toPoke, pokeMessage, 5000);
/*						char toPoke[strlen(message) + 1];
						getArg(toPoke, message, 1);
						char pokeMessage[strlen(message) + 1];
						getArg(pokeMessage, message, -2);
						if (pokeIsWorking) {
							pokeIsWorking = false;
							sendMessageToChannel("Stopped spamming! 8)");
						} else if (!pokeWorkerIsRunning()) {
							pokeIsWorking = true;
							toPokeID = getClientIDfromClientName(toPoke);
							if (unlikely(pthread_create(&pokeThread, NULL, &pokeWorker, (void*) NULL))) {
								sendErrorToChannel("pthread_create() error");
								return 1;
							} else {
								pthread_detach(pokeThread);
								sendMessageToChannel("Started spamming! 8)");
							}
						} else {
							sendMessageToChannel("Wait a moment! 8)");
						}*/
					}
				} else if (strcasecmp(message, "!prev") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc prev 2>&1");
					}
				} else if (strcasecmp(message, "!random") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc random 2>&1");
					}
				} else if (strcasecmp(message, "!randomfav") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						playFav(fromUniqueIdentifier, RANDOM, false);
						refreshFavSymlink(fromName, fromUniqueIdentifier);
					}
				} else if (strncasecmp(message, "!randomfav ", 11) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						playFav(messageSubstring, RANDOM, false);
					}
				} else if (strncasecmp(message, "!rankfav ", 9) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					rankFav(fromUniqueIdentifier, messageSubstring);
				} else if (strcasecmp(message, "!repeat") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc repeat 2>&1");
					}
				} else if (strcasecmp(message, "!reset") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						resetPlaylist();
					}
				} else if (strcasecmp(message, "!restart") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						sendErrorToChannel("Empty placeholder! :-("); // TODO
					}
				} else if (strncasecmp(message, "!say ", 5) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						sendMessageToChannel(messageSubstring);
					}
				} else if (strcasecmp(message, "!shuffle") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc shuffle 2>&1");
					}
				} else if (strcasecmp(message, "!single") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc single 2>&1");
					}
				} else if (strcasecmp(message, "!song") == 0) {
					executeCommandWithOutputToChannel("mpc -f \"Artist: %artist%\nAlbum: %album%\nTitle: %title%\nTheme: %comment%\nLength: %time%\" current 2>&1");
				} else if (strncasecmp(message, "!song ", 6) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getSong(messageSubstring, true);
				} else if (strcasecmp(message, "!songs") == 0) {
					executeCommandWithOutputToChannel("mpc listall 2>&1");
				} else if (strncasecmp(message, "!songs ", 7) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getSong(messageSubstring, false);
				} else if (strcasecmp(message, "!stats") == 0) {
					executeCommandWithOutputToChannel("mpc stats 2>&1");
				} else if (strcasecmp(message, "!status") == 0) {
					executeCommandWithOutputToChannel("mpc 2>&1");
				} else if (strcasecmp(message, "!stop") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc stop 2>&1");
					}
				} else if (strcasecmp(message, "!theme") == 0) {
					executeCommandWithOutputToChannel("mpc -f \"Theme: %comment%\" current 2>&1");
				} else if (strncasecmp(message, "!theme ", 7) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						setTheme(messageSubstring, false);
					}
				} else if (strncasecmp(message, "!themefixed ", 12) == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						char messageSubstring[strlen(message) + 1];
						getArg(messageSubstring, message, -1);
						setTheme(messageSubstring, true);
					}
				} else if (strcasecmp(message, "!themes") == 0) {
					getTheme(NULL);
				} else if (strncasecmp(message, "!themes ", 8) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					getTheme(messageSubstring);
				} else if (strcasecmp(message, "!unfav") == 0) {
					delFav(fromUniqueIdentifier);
					refreshFavSymlink(fromName, fromUniqueIdentifier);
				} else if (strcasecmp(message, "!update") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						sendMessageToChannel("Updating database...");
						executeCommandWithErrorToChannel("mpc update --wait >/dev/null");
						sendMessageToChannel("Done! 8)");
					}
				} else if (strcasecmp(message, "!version") == 0) {
					sendMessageToChannel("Archi's Music Bot V2.0");
					executeCommandWithOutputToChannel("mpc version 2>&1");
					executeCommandWithOutputToChannel("pulseaudio --version 2>&1");
				} else if (strcasecmp(message, "!vol-") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc volume -10 2>&1");
					}
				} else if (strcasecmp(message, "!vol+") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						executeCommandWithOutputToChannel("mpc volume +10 2>&1");
					}
				} else if (strcasecmp(message, "!zipfavs") == 0) {
					zipFav(fromUniqueIdentifier);
					refreshFavSymlink(fromName, fromUniqueIdentifier);
				} else if (strncasecmp(message, "!zipfavs ", 9) == 0) {
					char messageSubstring[strlen(message) + 1];
					getArg(messageSubstring, message, -1);
					zipFav(messageSubstring);
				} else if (strcasecmp(message, "!wypierdol") == 0) {
					if (isAccessGranted(fromID, rootGroup)) {
						delSong();
					}
				} else {
					sendErrorToChannel("Unknown command! :-(");
				}
			}
		}
	}

	return 1;  /* 0 = handle normally, 1 = client will ignore the text message */
}

//void ts3plugin_onTalkStatusChangeEvent(uint64 serverConnectionHandlerID, int status, int isReceivedWhisper, anyID clientID) {
//}

//void ts3plugin_onConnectionInfoEvent(uint64 serverConnectionHandlerID, anyID clientID) {
//}

//void ts3plugin_onServerConnectionInfoEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onChannelSubscribeEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
//}

//void ts3plugin_onChannelSubscribeFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onChannelUnsubscribeEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
//}

//void ts3plugin_onChannelUnsubscribeFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onChannelDescriptionUpdateEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
//}

//void ts3plugin_onChannelPasswordChangedEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
//}

//void ts3plugin_onPlaybackShutdownCompleteEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onSoundDeviceListChangedEvent(const char* modeID, int playOrCap) {
//}

//void ts3plugin_onEditPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels) {
//}

//void ts3plugin_onEditPostProcessVoiceDataEvent(uint64 serverConnectionHandlerID, anyID clientID, short* samples, int sampleCount, int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask) {
//}

//void ts3plugin_onEditMixedPlaybackVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, const unsigned int* channelSpeakerArray, unsigned int* channelFillMask) {
//}

//void ts3plugin_onEditCapturedVoiceDataEvent(uint64 serverConnectionHandlerID, short* samples, int sampleCount, int channels, int* edited) {
//}

//void ts3plugin_onCustom3dRolloffCalculationClientEvent(uint64 serverConnectionHandlerID, anyID clientID, float distance, float* volume) {
//}

//void ts3plugin_onCustom3dRolloffCalculationWaveEvent(uint64 serverConnectionHandlerID, uint64 waveHandle, float distance, float* volume) {
//}

//void ts3plugin_onUserLoggingMessageEvent(const char* logMessage, int logLevel, const char* logChannel, uint64 logID, const char* logTime, const char* completeLogString) {
//}

//void ts3plugin_onClientBanFromServerEvent(uint64 serverConnectionHandlerID, anyID clientID, uint64 oldChannelID, uint64 newChannelID, int visibility, anyID kickerID, const char* kickerName, const char* kickerUniqueIdentifier, uint64 time, const char* kickMessage) {
//}

int ts3plugin_onClientPokeEvent(uint64 serverConnectionHandlerID, anyID fromClientID, const char* pokerName, const char* pokerUniqueIdentity, const char* message, int ffIgnored) {
	if (ffIgnored) {
		return 1;
	}

	if (fromClientID != myID) {
		if (ts3Functions.requestClientPoke(serverConnectionHandlerID, fromClientID, "Don't poke me Senpai! :-(", NULL) != ERROR_ok) {
			sendErrorToChannel("requestClientPoke() error");
		}
	}

	return 1;
}

//void ts3plugin_onClientSelfVariableUpdateEvent(uint64 serverConnectionHandlerID, int flag, const char* oldValue, const char* newValue) {
//}

//void ts3plugin_onFileListEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* path, const char* name, uint64 size, uint64 datetime, int type, uint64 incompletesize, const char* returnCode) {
//}

//void ts3plugin_onFileListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* path) {
//}

//void ts3plugin_onFileInfoEvent(uint64 serverConnectionHandlerID, uint64 channelID, const char* name, uint64 size, uint64 datetime) {
//}

//void ts3plugin_onServerGroupListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, const char* name, int type, int iconID, int saveDB) {
//}

//void ts3plugin_onServerGroupListFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onServerGroupByClientIDEvent(uint64 serverConnectionHandlerID, const char* name, uint64 serverGroupList, uint64 clientDatabaseID) {
//}

//void ts3plugin_onServerGroupPermListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
//}

//void ts3plugin_onServerGroupPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID) {
//}

//void ts3plugin_onServerGroupClientListEvent(uint64 serverConnectionHandlerID, uint64 serverGroupID, uint64 clientDatabaseID, const char* clientNameIdentifier, const char* clientUniqueID) {
//}

//void ts3plugin_onChannelGroupListEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, const char* name, int type, int iconID, int saveDB) {
//}

//void ts3plugin_onChannelGroupListFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onChannelGroupPermListEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
//}

//void ts3plugin_onChannelGroupPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID) {
//}

//void ts3plugin_onChannelPermListEvent(uint64 serverConnectionHandlerID, uint64 channelID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
//}

//void ts3plugin_onChannelPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID) {
//}

//void ts3plugin_onClientPermListEvent(uint64 serverConnectionHandlerID, uint64 clientDatabaseID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
//}

//void ts3plugin_onClientPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 clientDatabaseID) {
//}

//void ts3plugin_onChannelClientPermListEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 clientDatabaseID, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
//}

//void ts3plugin_onChannelClientPermListFinishedEvent(uint64 serverConnectionHandlerID, uint64 channelID, uint64 clientDatabaseID) {
//}

//void ts3plugin_onClientChannelGroupChangedEvent(uint64 serverConnectionHandlerID, uint64 channelGroupID, uint64 channelID, anyID clientID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
//}

//int ts3plugin_onServerPermissionErrorEvent(uint64 serverConnectionHandlerID, const char* errorMessage, unsigned int error, const char* returnCode, unsigned int failedPermissionID) {
//	return 0;
//}

//void ts3plugin_onPermissionListGroupEndIDEvent(uint64 serverConnectionHandlerID, unsigned int groupEndID) {
//}

//void ts3plugin_onPermissionListEvent(uint64 serverConnectionHandlerID, unsigned int permissionID, const char* permissionName, const char* permissionDescription) {
//}

//void ts3plugin_onPermissionListFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onPermissionOverviewEvent(uint64 serverConnectionHandlerID, uint64 clientDatabaseID, uint64 channelID, int overviewType, uint64 overviewID1, uint64 overviewID2, unsigned int permissionID, int permissionValue, int permissionNegated, int permissionSkip) {
//}

//void ts3plugin_onPermissionOverviewFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onServerGroupClientAddedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity, uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
//}

//void ts3plugin_onServerGroupClientDeletedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientName, const char* clientUniqueIdentity, uint64 serverGroupID, anyID invokerClientID, const char* invokerName, const char* invokerUniqueIdentity) {
//}

//void ts3plugin_onClientNeededPermissionsEvent(uint64 serverConnectionHandlerID, unsigned int permissionID, int permissionValue) {
//}

//void ts3plugin_onClientNeededPermissionsFinishedEvent(uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onFileTransferStatusEvent(anyID transferID, unsigned int status, const char* statusMessage, uint64 remotefileSize, uint64 serverConnectionHandlerID) {
//}

//void ts3plugin_onClientChatClosedEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientUniqueIdentity) {
//}

//void ts3plugin_onClientChatComposingEvent(uint64 serverConnectionHandlerID, anyID clientID, const char* clientUniqueIdentity) {
//}

//void ts3plugin_onServerLogEvent(uint64 serverConnectionHandlerID, const char* logMsg) {
//}

//void ts3plugin_onServerLogFinishedEvent(uint64 serverConnectionHandlerID, uint64 lastPos, uint64 fileSize) {
//}

//void ts3plugin_onMessageListEvent(uint64 serverConnectionHandlerID, uint64 messageID, const char* fromClientUniqueIdentity, const char* subject, uint64 timestamp, int flagRead) {
//}

//void ts3plugin_onMessageGetEvent(uint64 serverConnectionHandlerID, uint64 messageID, const char* fromClientUniqueIdentity, const char* subject, const char* message, uint64 timestamp) {
//}

//void ts3plugin_onClientDBIDfromUIDEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, uint64 clientDatabaseID) {
//}

//void ts3plugin_onClientNamefromUIDEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, uint64 clientDatabaseID, const char* clientNickName) {
//}

//void ts3plugin_onClientNamefromDBIDEvent(uint64 serverConnectionHandlerID, const char* uniqueClientIdentifier, uint64 clientDatabaseID, const char* clientNickName) {
//}

//void ts3plugin_onComplainListEvent(uint64 serverConnectionHandlerID, uint64 targetClientDatabaseID, const char* targetClientNickName, uint64 fromClientDatabaseID, const char* fromClientNickName, const char* complainReason, uint64 timestamp) {
//}

//void ts3plugin_onBanListEvent(uint64 serverConnectionHandlerID, uint64 banid, const char* ip, const char* name, const char* uid, uint64 creationTime, uint64 durationTime, const char* invokerName, uint64 invokercldbid, const char* invokeruid, const char* reason, int numberOfEnforcements, const char* lastNickName) {
//}

//void ts3plugin_onClientServerQueryLoginPasswordEvent(uint64 serverConnectionHandlerID, const char* loginPassword) {
//}

//void ts3plugin_onPluginCommandEvent(uint64 serverConnectionHandlerID, const char* pluginName, const char* pluginCommand) {
//}

//void ts3plugin_onIncomingClientQueryEvent(uint64 serverConnectionHandlerID, const char* commandText) {
//}

//void ts3plugin_onServerTemporaryPasswordListEvent(uint64 serverConnectionHandlerID, const char* clientNickname, const char* uniqueClientIdentifier, const char* description, const char* password, uint64 timestampStart, uint64 timestampEnd, uint64 targetChannelID, const char* targetChannelPW) {
//}

/* Client UI callbacks */

//void ts3plugin_onAvatarUpdated(uint64 serverConnectionHandlerID, anyID clientID, const char* avatarPath) {
//}

//void ts3plugin_onMenuItemEvent(uint64 serverConnectionHandlerID, enum PluginMenuType type, int menuItemID, uint64 selectedItemID) {
//}

/* Called when client custom nickname changed */
void ts3plugin_onClientDisplayNameChanged(uint64 serverConnectionHandlerID, anyID clientID, const char* displayName, const char* uniqueClientIdentifier) {
	if (unlikely(requiresNickCorrection && clientID == myID)) {
		if (likely(strcmp(botNickname, displayName) == 0)) {
			requiresNickCorrection = false;
		}
	}
}
