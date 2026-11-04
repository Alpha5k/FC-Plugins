-- Preserve nil result slots from original callbacks.
local function net_screen_results(...) return arg end

-- Keep native fleet navigation; reserve selection and campaign actions for the commander.
function ifs_freeform_net_fnWrapFleet(this)
	for _, name in ipairs({ "AttemptMove", "AttemptUndo", "BuildFleet" }) do
		-- Lua 5.0 clears the generic-for name after the loop; each callback needs its own value.
		local action_name, previous = name, this[name]
		this[name] = function(this, ...)
			if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
				return ifs_freeform_net_fnFleetRequest(action_name, unpack(arg, 1, arg.n))
			end
			return previous(this, unpack(arg, 1, arg.n))
		end
	end

	local previous_select = this.SelectFleet
	this.SelectFleet = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing
			and not ifs_freeform_net_fnCanCommand() then
			return false
		end
		return previous_select(this, unpack(arg, 1, arg.n))
	end

	local previous_accept = this.Input_Accept
	this.Input_Accept = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing
			and this.CurButton == "_accept"
			and not ifs_freeform_net_fnCanCommand() then
			return
		end
		if ifs_freeform_net_campaign.active and this.CurButton == "_next" then
			return ifs_freeform_net_fnRequest("pass")
		end
		return previous_accept(this, unpack(arg, 1, arg.n))
	end

	local previous_back = this.Input_Back
	this.Input_Back = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing
			and ifs_freeform_net_fnMovePending() then
			return ifs_freeform_net_fnRequest("undo_move")
		end
		return previous_back(this, unpack(arg, 1, arg.n))
	end

	local previous_action = this.UpdateAction
	this.UpdateAction = function(this, ...)
		if ifs_freeform_net_campaign.active
			and (not ifs_freeform_net_fnCanCommand() or ifs_freeform_net_fnMovePending()) then
			this.fleetSelected = nil
		end

		local result = net_screen_results(previous_action(this, unpack(arg, 1, arg.n)))
		if ifs_freeform_net_campaign.active
			and (not ifs_freeform_net_fnCanCommand() or ifs_freeform_net_fnMovePending()) then
			this.buildFleet = nil
			if not ifs_freeform_net_fnCanCommand() then
				ifs_freeform_SetButtonVis(this, "back", false)
			end
			ifs_freeform_SetButtonVis(this, "accept", false)
		end

		-- Stock UpdateAction names Skip without restoring its visibility.
		ifs_freeform_SetButtonVis(this, "misc", not ifs_freeform_net_campaign.active
			or (ifs_freeform_net_fnCanCommand() and not ifs_freeform_net_fnMovePending()))
		return unpack(result, 1, result.n)
	end

	local previous_enter = this.Enter
	this.Enter = function(this, bFwd, ...)
		if not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.executing then
			return previous_enter(this, bFwd, unpack(arg, 1, arg.n))
		end

		local turn, start, next = this.turnNumber, this.planetStart, this.planetNext
		local result = net_screen_results(pcall(previous_enter, this, true, unpack(arg, 1, arg.n)))

		this.turnNumber, this.planetStart, this.planetNext = turn, start, next
		this.displayTimer = nil

		if not result[1] then
			error(result[2])
		end

		return unpack(result, 2, result.n)
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end

local previous_default_accept = gShellScreen_fnDefaultInputAccept

-- Stock purchase Accept reaches this checkpoint after tabs, help and carousel navigation.
function gShellScreen_fnDefaultInputAccept(this, ...)
	if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing
		and this == ifs_freeform_purchase_unit then
		if previous_default_accept(this, unpack(arg, 1, arg.n)) then
			return true
		end
		local item = this.purchaseItems[this.selected]
		if not item or not ifs_purchase_unit_table[item.itemtype] then
			return true
		end

		-- Let the original callback play the feedback for owned or unaffordable troops.
		if ifs_purchase_unit_owned[this.main.playerTeam][item.itemtype]
			or not this.main:EnoughResources(nil, ifs_purchase_unit_cost[item.itemtype]) then
			return false
		end
		if not ifs_freeform_net_fnCanRecruit() then
			return true
		end
		ifs_freeform_net_fnRequest("recruit", item.itemtype)
		-- Finish handling this input here; the host applies the purchase.
		return true
	end

	if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing
		and this == ifs_freeform_purchase_tech then
		-- Original keyboard and double-click paths both reach this checkpoint after navigation.
		local button = this.CurButton
		this.CurButton = "_accept"
		this:Input_Accept()
		this.CurButton = button
		return true
	end

	return previous_default_accept(this, unpack(arg, 1, arg.n))
end

-- Keep stock purchase navigation and replacement prompts; submit confirmed choices to the host.
function ifs_freeform_net_fnWrapPurchase(this, units)
	local previous_accept, previous_back, previous_exit = this.Input_Accept, this.Input_Back, this.Exit
	this.Input_Accept = function(this, joystick, ...)
		-- Run troop purchase input through the original navigation and input checks.
		if units or not ifs_freeform_net_campaign.active or ifs_freeform_net_campaign.executing then
			return previous_accept(this, joystick, unpack(arg, 1, arg.n))
		end
		if this.CurButton ~= "_accept" then
			return previous_accept(this, joystick, unpack(arg, 1, arg.n))
		end
		if not ifs_freeform_net_fnCanRecruit() then
			return
		end

		local state = ifs_freeform_net_campaign.state
		if (state.pendingCard or 0) > 0 then
			local position = this.useItems.cursor.position or 1
			local slots = ifs_purchase_tech_using[state.decisionFaction]
			if slots[position] ~= 0 then
				-- Keep the original replacement popup, replacing only its accepted continuation.
				this:PromptReplace(state.decisionFaction, position, state.pendingCard)
				local done = Popup_YesNo.fnDone
				Popup_YesNo.fnDone = function(accepted)
					if not accepted then
						return done(false)
					end
					ifs_freeform_net_fnRequest("place_card", "", "", position, true)
					-- Stock Yes restores these controls; the acknowledgement supplies its purchase sound.
					IFObj_fnSetVis(this.purchaseItems, true)
					IFObj_fnSetVis(this.useItems, true)
				end
				return
			end
			return ifs_freeform_net_fnRequest("place_card", "", "", position)
		end

		local owned = ifs_purchase_tech_cards[state.localFaction][this.selected]
		if not this.main:EnoughResources(state.localFaction,
			ifs_purchase_tech_table[this.selected].cost[owned]) then
			ifelm_shellscreen_fnPlaySound(this.cancelSound)
			this.main:PlayVoice(string.format(ifs_purchase_tech_broke_sound[owned], this.main.playerSide))
			return
		end
		return ifs_freeform_net_fnRequest("buy_card", "", "", this.selected)
	end

	this.Input_Back = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing and not units
			and (ifs_freeform_net_campaign.state.pendingCard or 0) > 0 then
			if ifs_freeform_net_fnCanCommand() then
				return ifs_freeform_net_fnRequest("cancel_card")
			end

			-- A viewer can leave while another commander retains the paid placement.
			local focus = this.focus
			this.focus = ifs_purchase_tech_focus_cards
			local result = net_screen_results(pcall(previous_back, this, unpack(arg, 1, arg.n)))

			this.focus = focus

			if not result[1] then
				error(result[2])
			end

			return unpack(result, 2, result.n)
		end
		return previous_back(this, unpack(arg, 1, arg.n))
	end

	this.Exit = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing and not units then
			local focus = this.focus
			this.focus = ifs_purchase_tech_focus_cards
			local result = net_screen_results(pcall(previous_exit, this, unpack(arg, 1, arg.n)))

			this.focus = focus

			if not result[1] then
				error(result[2])
			end

			return unpack(result, 2, result.n)
		end
		return previous_exit(this, unpack(arg, 1, arg.n))
	end

	local previous_enter = this.Enter
	this.Enter = function(this, ...)
		if ifs_freeform_net_campaign.active and not ifs_freeform_net_campaign.executing then
			ifs_freeform_net_fnSetViewFaction()
		end
		return previous_enter(this, unpack(arg, 1, arg.n))
	end

	local actions = units and { "UpdateAction" } or { "UpdateActionCarousel", "UpdateActionUsing" }
	for _, action_name in ipairs(actions) do
		local previous_action = this[action_name]
		this[action_name] = function(this, ...)
			local result = net_screen_results(previous_action(this, unpack(arg, 1, arg.n)))
			-- Stock card controls update their dimming without restoring visibility.
			ifs_freeform_SetButtonVis(this, "accept",
				not ifs_freeform_net_campaign.active or ifs_freeform_net_fnCanRecruit())
			return unpack(result, 1, result.n)
		end
	end

	ifs_freeform_net_fnWrapSessionUpdate(this)
end
