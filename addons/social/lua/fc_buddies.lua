-- Preserve callback results while updating the Galaxy relationship glyph.
local function fc_buddies_Icon(row, player, ...)
	local icons = {accepted = 1, incoming = 2, outgoing = 3}
	if row.StateIcon then
		local icon
		if player then
			local muted, friend, boot, guest, add, relationship =
				ScriptCB_GetLobbyPlayerFlags(player.namestr, player.indexstr)
			icon = icons[relationship]
		end
		if icon then
			local uv = gXLFriendsEnum2UVs[icon + 1]
			IFImage_fnSetUVs(row.StateIcon, uv.u, uv.v, uv.u + 0.25, uv.v + 0.25)
		end
		IFObj_fnSetVis(row.StateIcon, icon)
	end
	return unpack(arg, 1, arg.n)
end

-- Add access to the stock friends screen during a session.
local function fc_buddies_Lobby(screen)
	if ScriptCB_GetShellActive() and not ifs_mpgs_friends then
		ScriptCB_DoFile("ifs_mpgs_friends")
	end
	screen.Helptext_Buddies = NewPCIFButton {
		ScreenRelativeX = 0.5, ScreenRelativeY = 1,
		y = -45, btnw = 200,
		btnh = ScriptCB_GetFontHeight("gamefont_small"), font = "gamefont_small",
		tag = "buddies", string = "ifs.onlinelobby.friendslist",
	}
	for index = 1, lobby_listbox_layout.showcount do
		local row = screen.listbox[index]
		if row.StateIcon then
			row.StateIcon.y = 4
		end
	end

	local populate = lobby_listbox_layout.PopulateFn
	ifs_mp_lobby_Listbox_PopulateItem = function(row, player, ...)
		return fc_buddies_Icon(row, player, populate(row, player, unpack(arg, 1, arg.n)))
	end
	lobby_listbox_layout.PopulateFn = ifs_mp_lobby_Listbox_PopulateItem
	local accept = screen.Input_Accept
	screen.Input_Accept = function(this, ...)
		if this.CurButton == "buddies" then
			ifs_mpgs_friends.bRecentMode = nil
			return ifs_movietrans_PushScreen(ifs_mpgs_friends)
		end
		return accept(this, unpack(arg, 1, arg.n))
	end

	-- Galaxy supplies a lobby identity rather than a useful public IP address.
	local function finish_enter(this, ...)
		IFObj_fnSetVis(this.IPAddr, nil)
		return unpack(arg, 1, arg.n)
	end
	local enter = screen.Enter
	screen.Enter = function(this, ...)
		return finish_enter(this, enter(this, unpack(arg, 1, arg.n)))
	end
end

-- Enter the stock join flow only after Galaxy accepts the selected friend's lobby.
local function fc_buddies_Join(this)
	local shell = ScriptCB_GetShellActive()
	if shell then
		ScriptCB_SetAmHost(nil)
		ScriptCB_SetDedicated(nil)
	end
	if not ScriptCB_DoFriendAction(this.CurButton, this.bRecentMode, this.BuddyActionTarget) then
		return
	end
	if not shell then
		ScriptCB_QuitToShell()
		return
	end

	AnimationMgr_AddAnimation(this.buttons, {fTotalTime = 0.2, fStartAlpha = 1, fEndAlpha = 0})
	AnimationMgr_AddAnimation(this.infobox, {fTotalTime = 0.2, fStartAlpha = 1, fEndAlpha = 0})
	this.bShowListbox = 1
	ifs_mpgs_friends_fnShowListbox(this, 1, 1)
	IFObj_fnSetVis(this.listbox, nil)
	Popup_Busy.fnCheckDone = ifs_mpgs_joinpopup_fnCheckDone
	Popup_Busy.fnOnSuccess = ifs_mpgs_joinpopup_fnOnSuccess
	Popup_Busy.fnOnFail = ifs_mpgs_joinpopup_fnOnFail
	Popup_Busy.fnOnCancel = ifs_mpgs_joinpopup_fnOnCancel
	Popup_Busy.fTimeout = 30
	IFText_fnSetString(Popup_Busy.title, "common.mp.joining")
	Popup_Busy:fnActivate(1)
end

-- Retain a selected account through confirmation and expose cancellation of outgoing requests.
local function fc_buddies_Friends(screen)
	local accept = screen.Input_Accept
	screen.Input_Accept = function(this, ...)
		this.BuddyActionTarget = friends_listbox_contents[gsfriends_listbox_layout.SelectedIdx]
		return accept(this, unpack(arg, 1, arg.n))
	end
	local action = ScriptCB_DoFriendAction
	ScriptCB_DoFriendAction = function(command, recent, target, ...)
		if FusionCutterFriendsActive then
			target = target or ifs_mpgs_friends.BuddyActionTarget
		end
		return action(command, recent, target, unpack(arg, 1, arg.n))
	end
	local join = ifs_mpgs_fnStartJoin
	ifs_mpgs_fnStartJoin = function(this, ...)
		if FusionCutterFriendsActive then
			return fc_buddies_Join(this)
		end
		return join(this, unpack(arg, 1, arg.n))
	end

	local buttons = ifs_mpgs_friends_fnPCShowHideButton
	local function finish_buttons(this, ...)
		if FusionCutterFriendsActive and not gCurEditbox and table.getn(friends_listbox_contents) > 0 then
			local actions = ScriptCB_GetFriendActions(this.bRecentMode)
			if actions.CANCEL then
				this.friend_buttons.remove_btn.tag = "CANCEL"
				RoundIFButtonLabel_fnSetString(this.friend_buttons.remove_btn, "common.cancel")
				IFObj_fnSetVis(this.friend_buttons.remove_btn, 1)
			end
		end
		return unpack(arg, 1, arg.n)
	end
	ifs_mpgs_friends_fnPCShowHideButton = function(this, ...)
		this.friend_buttons.remove_btn.tag = "REMOVE"
		RoundIFButtonLabel_fnSetString(this.friend_buttons.remove_btn, "ifs.mp.friends.remove")
		return finish_buttons(this, buttons(this, unpack(arg, 1, arg.n)))
	end
end

-- Extend Galaxy screens while leaving their original controls and navigation in place.
function fc_buddies_Extend(screen, name)
	if gOnlineServiceStr ~= "Galaxy" or gPlatformStr ~= "PC" or screen.fcBuddies then
		return
	end
	if name == "ifs_mp_lobby" then
		fc_buddies_Lobby(screen)
	elseif name == "ifs_mpgs_friends" then
		fc_buddies_Friends(screen)
	else
		return
	end
	screen.fcBuddies = 1
end
