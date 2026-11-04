if fc_social_loaded then
	return
end
fc_social_loaded = true

ScriptCB_DoFile("fc_voice_options")
ScriptCB_DoFile("fc_voice_lobby")
ScriptCB_DoFile("fc_buddies")

-- Extend each screen before the game creates its native controls.
local function fc_social_Install()
	local addScreen = AddIFScreen
	AddIFScreen = function(screen, name, ...)
		if type(ScriptCB_FC_GetVoiceLobbyGlyph) == "function" and gPlatformStr == "PC" then
			fc_voice_options_Extend(screen, name)
			if name == "ifs_mp_lobby" then
				fc_voice_lobby_Extend(screen)
			end
		end
		fc_buddies_Extend(screen, name)
		return addScreen(screen, name, unpack(arg, 1, arg.n))
	end
end

if AddIFScreen then
	fc_social_Install()
else
	-- The game entry runs before interface_util; the shell entry runs after it.
	local doFile = ScriptCB_DoFile
	local function finish_file(name, ...)
		if name == "interface_util" then
			fc_social_Install()
		end
		return unpack(arg, 1, arg.n)
	end
	ScriptCB_DoFile = function(name, ...)
		return finish_file(name, doFile(name, unpack(arg, 1, arg.n)))
	end
end
