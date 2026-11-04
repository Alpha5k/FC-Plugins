-- Stock refresh keeps Game Info rows; discard them when leaving or changing the browser source.
function ifs_freeform_net_fnClearBrowserDetails(this)
	mpsessionlist_serverinfo_contents = {}
	mpsessionlist_serverinfo_layout.FirstShownIdx = 1
	mpsessionlist_serverinfo_layout.SelectedIdx, mpsessionlist_serverinfo_layout.CursorIdx = nil, nil
	ListManager_fnFillContents(this.serverinfo,
		mpsessionlist_serverinfo_contents, mpsessionlist_serverinfo_layout)
end

-- Select native discovery and clear Game Info before stock source refresh or screen entry.
function ifs_freeform_net_fnSetBrowser(this)
	ScriptCB_SetMetagameBrowser(this.mpgc_selected == true)
	if this.mpgc_refresh_browser then
		this.mpgc_refresh_browser()
		ifs_freeform_net_fnClearBrowserDetails(this)
	end
end

-- Supply confirmed GC fields to the original Game Info rows, keeping native option text intact.
function ifs_freeform_net_fnBrowserDetails(native, info, selection)
	local details = {}
	if not info then
		return details
	end

	if native[1] and native[1].text then
		table.insert(details, native[1])
	elseif selection then
		table.insert(details, { text = ScriptCB_tounicode(selection.namestr) })
	end

	if info.campaign then
		local keys = {
			all = "ifs.meta.Configs.1",
			cis = "ifs.meta.Configs.2",
			rep = "ifs.meta.Configs.3",
			imp = "ifs.meta.Configs.4",
			custom = "ifs.meta.Configs.custom"
		}
		local key = keys[info.campaign]
		local campaign = key and ScriptCB_ununicode(ScriptCB_getlocalizestr(key)) or info.campaign
		table.insert(details, { text = ScriptCB_tounicode("Campaign: " .. campaign) })
	end
	if info.factions then
		table.insert(details, { text = ScriptCB_tounicode("Factions: " .. info.factions) })
	end
	if info.victoryCondition then
		table.insert(details, { text = ScriptCB_tounicode("Victory Condition: " .. info.victoryCondition) })
	end
	if info.turn ~= nil then
		table.insert(details, { text = ScriptCB_tounicode("Turn: " .. info.turn) })
	end
	if info.difficulty then
		table.insert(details, { text = ScriptCB_tounicode("AI Difficulty Level: " .. info.difficulty) })
	end
	if info.teamDamage ~= nil then
		local damage = ScriptCB_ununicode(ScriptCB_getlocalizestr(info.teamDamage and "common.on" or "common.off"))
		table.insert(details, { text = ScriptCB_tounicode("Team Damage: " .. damage) })
	end

	-- Retail supplies Password and Version; keep their native text and the separate player panel.
	for _, index in ipairs({ 11, 12 }) do
		if native[index] and native[index].text then
			table.insert(details, native[index])
		end
	end

	return details
end

-- Repurpose stock browser fields without changing their geometry or callbacks.
function ifs_freeform_net_fnBuildBrowser(this)
	local layout = mpsessionlist_listbox_layout
	local applied = false
	local hidden = {
		this.ResortButtons.favoriteLabel, this.ResortButtons.servertypeLabelLabel,
		this.DropBoxes.filter_servertypeLabel
	}
	local ordinary_visibility = {}
	local ordinary_join_dimmed

	-- Stock rendering retains native names, latency, join fields and password/version indicators.
	local previous_populate = layout.PopulateFn
	layout.PopulateFn = function(row, data, ...)
		local function finish(...)
			IFObj_fnSetVis(row.servertype, not this.mpgc_selected)
			if this.mpgc_selected then
				IFObj_fnSetVis(row.isfavorite, nil)
			end

			if this.mpgc_selected and data and not gBlankListbox then
				-- sessionIdx identifies the native session; Lua's sorted table position and player indexstr do not.
				local info = ScriptCB_GetMetagameSessionInfo(data.sessionIdx)
				local map, source = ScriptCB_tounicode(info and info.map or ""), 1
				if info and info.map ~= "Galaxy Map" and info.map ~= "" then
					map, source = missionlist_GetLocalizedMapName(info.map)
					if source == 2 then
						map = ScriptCB_usprintf("ifs.mp.sessionlist.map_missing", map)
					end
				end

				IFText_fnSetUString(row.map, map)
				IFObj_fnSetColor(row.map, 255, 255, source == 2 and 0 or 255)
				IFText_fnSetUString(row.numplayers,
					ScriptCB_tounicode(info and (info.count .. "/" .. info.maximum) or ""))
				IFText_fnSetUString(row.gamemode, ScriptCB_tounicode(info and info.mode or ""))
				IFText_fnSetString(row.eraname, info and info.era or "")
				IFObj_fnSetVis(row.eraname, true)
				IFObj_fnSetVis(row.eraicon1, nil)
				IFObj_fnSetVis(row.eraicon2, nil)

				for _, era in ipairs(gMapEras) do
					if info and info.era == era.subst then
						IFText_fnSetString(row.eraname, "")
						IFObj_fnSetVis(row.eraname, nil)
						IFImage_fnSetTexture(row.eraicon2, era.icon2)
						IFObj_fnSetVis(row.eraicon2, true)
					end
				end
			end

			return unpack(arg, 1, arg.n)
		end

		return finish(previous_populate(row, data, unpack(arg, 1, arg.n)))
	end

	local gc_maps = {
		{ showstr = "ifs.gsprofile.all", mapluafile = "ifs.gsprofile.all" },
		{ showstr = "Galaxy Map", mapluafile = "", mpgc_map = "Galaxy Map" }
	}
	local gc_modes = {
		{ string = "ifs.gsprofile.all", subst = "ifs.gsprofile.all" },
		{ string = "Co-op", subst = "Co-op" },
		{ string = "Versus", subst = "Versus" }
	}
	local gc_eras = { { text = "ifs.gsprofile.all" } }
	for _, era in ipairs(gMapEras) do
		if era.subst == "c" or era.subst == "g" then
			table.insert(gc_eras, { text = era.showstr, mpgc_era = era.subst })
		end
	end

	local dropdowns = {
		{
			name = "mpsessionlist_maplist_contents",
			layout = mpsessionlist_maplist_layout,
			box = this.maplistbox,
			gc = gc_maps
		},
		{
			name = "mpsessionlist_gamemodelist_contents",
			layout = mpsessionlist_gamemodelist_layout,
			box = this.gamemode_listbox,
			gc = gc_modes
		},
		{
			name = "mpsessionlist_eratypelist_contents",
			layout = mpsessionlist_eratypelist_layout,
			box = this.eralistbox,
			gc = gc_eras
		},
	}
	for _, entry in ipairs(dropdowns) do
		entry.ordinary, entry.ordinary_state, entry.gc_state = _G[entry.name], {}, { selected = 1, first = 1 }
	end
	local edit_tags = { "gamename", "numplayers", "ping" }
	local ordinary_edits, gc_edits = {}, {}

	-- Change the existing dropdown contents while retaining stock controls and layout.
	local function change_filters(selected)
		for _, entry in ipairs(dropdowns) do
			local current, saved = entry.layout, applied and entry.gc_state or entry.ordinary_state
			saved.selected, saved.cursor, saved.first =
				current.SelectedIdx, current.CursorIdx, current.FirstShownIdx
			local next_state = selected and entry.gc_state or entry.ordinary_state
			_G[entry.name] = selected and entry.gc or entry.ordinary
			current.SelectedIdx, current.CursorIdx, current.FirstShownIdx =
				next_state.selected or 1, next_state.cursor, next_state.first or 1
			ListManager_fnFillContents(entry.box, _G[entry.name], current)
		end

		for _, tag in ipairs(edit_tags) do
			local edit = this.DropBoxes["filter_" .. tag]
			local saved = applied and gc_edits or ordinary_edits
			saved[tag] = IFEditbox_fnGetString(edit)
			if not gc_edits[tag] then
				gc_edits[tag] = ordinary_edits[tag]
			end
			IFEditbox_fnSetString(edit, (selected and gc_edits or ordinary_edits)[tag])
		end
	end

	local previous_map = mpsessionlist_maplist_layout.PopulateFn
	mpsessionlist_maplist_layout.PopulateFn = function(row, data, ...)
		if data and data.mpgc_map and not gBlankListbox then
			IFText_fnSetUString(row.text, ScriptCB_tounicode(data.mpgc_map))
			IFObj_fnSetVis(row.text, true)
			return
		end

		return previous_map(row, data, unpack(arg, 1, arg.n))
	end

	local previous_mode = ifs_mp_sessionlist_fnUpdateFilterGameMode
	ifs_mp_sessionlist_fnUpdateFilterGameMode = function(screen, ...)
		if not screen.mpgc_selected then
			return previous_mode(screen, unpack(arg, 1, arg.n))
		end

		IFObj_fnSetVis(screen.GameMode_Filter_Image, nil)
		IFObj_fnSetVis(screen.DropBoxes.filter_gamemode.showtext, true)
	end

	local previous_text = ifs_mp_sessionlist_fnUpdateFilterText
	ifs_mp_sessionlist_fnUpdateFilterText = function(screen, ...)
		local function finish(...)
			if screen.mpgc_selected and mpsessionlist_maplist_layout.SelectedIdx == 2 then
				IFText_fnSetUString(screen.DropBoxes.filter_mapname.showtext, ScriptCB_tounicode("Galaxy Map"))
			end

			return unpack(arg, 1, arg.n)
		end

		return finish(previous_text(screen, unpack(arg, 1, arg.n)))
	end

	local previous_filters = ifs_mp_sessionlist_fnApplyFilters
	ifs_mp_sessionlist_fnApplyFilters = function(screen, ...)
		if not screen.mpgc_selected then
			return previous_filters(screen, unpack(arg, 1, arg.n))
		end

		-- Native GC predicates use campaign metadata; mission fields remain the original All sentinels.
		local map = mpsessionlist_maplist_contents[mpsessionlist_maplist_layout.SelectedIdx]
		local mode = mpsessionlist_gamemodelist_contents[mpsessionlist_gamemodelist_layout.SelectedIdx]
		local era = mpsessionlist_eratypelist_contents[mpsessionlist_eratypelist_layout.SelectedIdx]
		ScriptCB_SetMetagameFilters(map.mpgc_map or "",
			mode.subst == "ifs.gsprofile.all" and "" or mode.subst, era.mpgc_era or "")
		ScriptCB_ApplyFilters(IFEditbox_fnGetString(screen.DropBoxes.filter_gamename),
			IFEditbox_fnGetString(screen.DropBoxes.filter_numplayers), mpsessionlist_maplist_contents[1].mapluafile,
			mpsessionlist_eratypelist_contents[1].text, IFEditbox_fnGetString(screen.DropBoxes.filter_ping),
			mpsessionlist_servertypelist_contents[1].text, mpsessionlist_gamemodelist_contents[1].subst)
	end

	-- Native source selection restores sorting; Lua restores controls before stock refresh restarts discovery.
	this.mpgc_refresh_browser = function()
		local selected = this.mpgc_selected == true
		if applied ~= selected then
			change_filters(selected)
			if selected then
				ordinary_join_dimmed = this.donebutton.bDimmed
				for _, control in ipairs(hidden) do
					ordinary_visibility[control] = IFObj_fnGetVis(control)
				end
			else
				for control, visible in pairs(ordinary_visibility) do
					IFObj_fnSetVis(control, visible)
				end
				this.donebutton.bDimmed = ordinary_join_dimmed
				IFObj_fnSetAlpha(this.donebutton, ordinary_join_dimmed and 0.3 or 1)
				IFObj_fnSetAlpha(this.donebutton.label, ordinary_join_dimmed and 0.3 or 1)
			end

			applied = selected
			ifs_mp_sessionlist_fnUpdateFilterText(this)
		end

		if selected then
			for _, control in ipairs(hidden) do
				IFObj_fnSetVis(control, nil)
			end

			local selection = mpsessionlist_listbox_contents[mpsessionlist_listbox_layout.SelectedIdx or 0]
			local info = selection and ScriptCB_GetMetagameSessionInfo(selection.sessionIdx)
			local disabled = not info or not info.joinable
			this.donebutton.bDimmed = disabled or nil
			IFObj_fnSetAlpha(this.donebutton, disabled and 0.3 or 1)
			IFObj_fnSetAlpha(this.donebutton.label, disabled and 0.3 or 1)
		end
	end

	-- Every stock join gesture reaches this helper, including double-clicks and password submission.
	local previous_join = ifs_mp_sessionlist_fnStartJoin
	ifs_mp_sessionlist_fnStartJoin = function(screen, ...)
		if screen.mpgc_selected then
			local selection = mpsessionlist_listbox_contents[mpsessionlist_listbox_layout.SelectedIdx or 0]
			local info = selection and ScriptCB_GetMetagameSessionInfo(selection.sessionIdx)
			if not info or not info.joinable then
				return
			end
		end

		return previous_join(screen, unpack(arg, 1, arg.n))
	end

	local previous_accept = this.Input_Accept
	this.Input_Accept = function(this, ...)
		if this.CurButton and string.sub(this.CurButton, 1, 5) == "_tab_" then
			ifs_freeform_net_fnClearBrowserDetails(this)
		end

		return previous_accept(this, unpack(arg, 1, arg.n))
	end

	local previous_exit = this.Exit
	this.Exit = function(this, ...)
		ifs_freeform_net_fnClearBrowserDetails(this)
		return previous_exit(this, unpack(arg, 1, arg.n))
	end

	local previous_extra = this.RepaintExtraInfo
	this.RepaintExtraInfo = function(this, ...)
		if not this.mpgc_selected then
			return previous_extra(this, unpack(arg, 1, arg.n))
		end

		local native = mpsessionlist_serverinfo_contents
		local selection = mpsessionlist_listbox_contents[mpsessionlist_listbox_layout.SelectedIdx or 0]
		local info = selection and ScriptCB_GetMetagameSessionInfo(selection.sessionIdx)
		mpsessionlist_serverinfo_contents = ifs_freeform_net_fnBrowserDetails(native, info, selection)

		-- ListManager keeps the adapted rows; restore the native table after its original repaint callback.
		local function results(...)
			return arg
		end
		local returned = results(pcall(previous_extra, this, unpack(arg, 1, arg.n)))

		mpsessionlist_serverinfo_contents = native

		if not returned[1] then
			error(returned[2])
		end

		return unpack(returned, 2, returned.n)
	end

	local previous_update = this.Update
	this.Update = function(this, ...)
		local function finish(...)
			this.mpgc_refresh_browser()
			return unpack(arg, 1, arg.n)
		end

		return finish(previous_update(this, unpack(arg, 1, arg.n)))
	end
end
