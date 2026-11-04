-- Paint the existing player-list glyph from FC's local or received voice activity.
local function fc_voice_lobby_Icon(icon, player)
	local status = player and ScriptCB_FC_GetVoiceLobbyGlyph(player.indexstr)
	local states = player and player.bIsLocal and gVoiceStatus.send or gVoiceStatus.receive
	local entry = status and states[status]
	if not entry then
		IFObj_fnSetVis(icon, nil)
		return
	end

	local lobby = ifs_mp_lobby
	local flash = math.min(lobby.flashNextTime - lobby.flashTimeElapsed, 1) / lobby.flashInterval
	ifs_mp_lobby_Listbox_DisplayVoiceIcon(icon, entry, flash > 0.5 and 1 or 0)
end

-- Voice activity can change without a native roster update.
local function fc_voice_lobby_Refresh(screen, ...)
	local first = lobby_listbox_layout.FirstShownIdx or 1
	for index = 1, lobby_listbox_layout.showcount do
		local row = screen.listbox[index]
		if row and row.VoiceIcon then
			fc_voice_lobby_Icon(row.VoiceIcon, ifs_mp_lobby_listbox_contents[first + index - 1])
		end
	end
	return unpack(arg, 1, arg.n)
end

-- Keep the stock columns and hook both the cached row painter and the screen update.
function fc_voice_lobby_Extend(screen)
	for index = 1, lobby_listbox_layout.showcount do
		local row = screen.listbox[index]
		if row.VoiceIcon then
			row.VoiceIcon.y = 4
		end
	end

	local populate = lobby_listbox_layout.PopulateFn
	local function finish_row(row, player, ...)
		if row.VoiceIcon then
			fc_voice_lobby_Icon(row.VoiceIcon, player)
		end
		return unpack(arg, 1, arg.n)
	end
	ifs_mp_lobby_Listbox_PopulateItem = function(row, player, ...)
		return finish_row(row, player, populate(row, player, unpack(arg, 1, arg.n)))
	end
	lobby_listbox_layout.PopulateFn = ifs_mp_lobby_Listbox_PopulateItem

	local update = screen.Update
	screen.Update = function(this, ...)
		return fc_voice_lobby_Refresh(this, update(this, unpack(arg, 1, arg.n)))
	end
end
