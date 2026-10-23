// Unlayered 2026-10-09: the Bambu Lab network plug-in page (see index.html). Stealth mode was asked
// here too until 2026-10-22. The answer is reported on load (so the defaults count even if nothing is clicked), on every
// click, and once more just before Finish.

function OnInit()
{
	TranslatePage();

	SendInstallPluginCheck();

	// Unlayered 2026-10-21: ticked when another slicer has Bambu Lab printers (see index.html).
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_network_plugin_hint";
	SendWXMessage( JSON.stringify(tSend) );
}

var m_PluginTouched=false;
function HandleStudio( pVal )
{
	if( pVal['command']!='network_plugin_hint' || !pVal['tick'] || m_PluginTouched )
		return;

	$('#InstallCheck').prop('checked', true);
	$('#PluginHintText').text(pVal['text']);
	$('#PluginHint').prop('hidden', false);
	// Report it like a click, so the ticked state is what the slicer has (FinishGuide sends it again).
	SendInstallPluginCheck();
}

function SendInstallPluginCheck()
{
	let ev=window.event;
	if( ev && ev.type=='click' )
		m_PluginTouched=true; // the person's own choice wins over the hint
	let nVal="no";
	if( $('#InstallCheck').is(':checked') )
		nVal="yes";

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="network_plugin_install";
	tSend['data']={};
	tSend['data']['action']=nVal;

	SendWXMessage( JSON.stringify(tSend) );

	return true;
}

function FinishGuide()
{
	SendInstallPluginCheck();

	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="user_guide_finish";
	tSend['data']={};
	tSend['data']['action']="finish";

	SendWXMessage( JSON.stringify(tSend) );
}
