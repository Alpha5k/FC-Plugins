-- Close owned native displays before the stock state transition tears down the Lua VM.
local previous_state = SetState
SetState = function(state, ...)
	ScriptCB_SetMetagameChatActive(false)
	return previous_state(state, unpack(arg, 1, arg.n))
end

-- Screen transitions cancel the original editor and retain the native message history.
function ifs_freeform_net_fnCloseChat()
	ScriptCB_CloseMetagameChatInput()
end

-- The game handles configured Say/SayTeam bindings, editing and submission.
-- Only the GC player-list destination needs a shell callback.
function ifs_freeform_net_fnSessionInput(this)
	if not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.failed then
		return
	end

	local input = ScriptCB_GetMetagameInput()
	if not input then
		return
	end

	local players = input.player_list and not ifs_freeform_net_campaign.playerListHeld
	ifs_freeform_net_campaign.playerListHeld = input.player_list
	if input.consumed then
		return true
	end

	if players then
		if this == ifs_mp_lobby then
			ScriptCB_PopScreen()
		else
			ScriptCB_PushScreen("ifs_mp_lobby")
		end
		return true
	end
end

-- Load only the extracted original MultilineText after its native factories/events exist.
function ifs_freeform_net_fnUpdateChat(this, dt)
	if not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.failed then
		ScriptCB_SetMetagameChatActive(false)
		ifs_freeform_net_campaign.playerListHeld = nil
		return
	end

	local input = ScriptCB_GetMetagameInput()
	if not input or not input.chat_open then
		local ready, message = ScriptCB_SetMetagameChatActive(true)
		if not ready then
			ifs_freeform_net_fnSessionError(message or "The native chat display could not be opened.")
			return
		end

		ReadDataFile("..\\..\\addon\\MPGC\\data\\_lvl_pc\\mpgc_chat.lvl")
	end

	ScriptCB_UpdateMetagameChat(dt)
	ifs_freeform_net_fnSessionInput(this)
end

-- Keep the stock screen callbacks while the native editor owns keyboard input.
function ifs_freeform_net_fnBuildChat(this)
	local previous_key = this.Input_KeyDown
	this.Input_KeyDown = function(this, key, ...)
		if ifs_freeform_net_fnSessionInput(this) then
			return
		end

		if previous_key then
			return previous_key(this, key, unpack(arg, 1, arg.n))
		end
	end

	for _, name in ipairs({
		"Input_Accept", "Input_Back", "Input_Start", "Input_Misc", "Input_Misc2",
		"Input_GeneralLeft", "Input_GeneralRight", "Input_GeneralUp", "Input_GeneralDown",
		"Input_LTrigger", "Input_RTrigger", "Input_LTrigger2", "Input_RTrigger2",
		"Input_DPadUp", "Input_DPadDown",
	}) do
		local previous = this[name]
		if previous then
			this[name] = function(this, ...)
				if ifs_freeform_net_fnSessionInput(this) then
					return
				end

				return previous(this, unpack(arg, 1, arg.n))
			end
		end
	end

	local previous_exit = this.Exit
	this.Exit = function(this, ...)
		ifs_freeform_net_fnCloseChat()
		if previous_exit then
			return previous_exit(this, unpack(arg, 1, arg.n))
		end
	end
end
