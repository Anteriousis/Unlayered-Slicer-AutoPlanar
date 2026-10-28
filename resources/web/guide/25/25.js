// Wizard step: offer the bundled multiplanar walkthrough, and say where to find it later.
// One way forward: Next (Skip did the same as Next, so it is gone).
// The tutorial opens in the default browser, so the wizard does not wait for it. This is NOT the last
// page (it used to say "Finish" and people missed the network plug-in question): Next goes on to
// guide 4unlayered, which has the only Finish button.

function OnInit()
{
	TranslatePage();
}

function OpenTutorial()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="open_multiplanar_tutorial";

	SendWXMessage( JSON.stringify(tSend) );
}

function ReturnFilamentPage()
{
	document.location.href="../22/index.html";
}

// Unlayered 2026-10-09: no longer the last page. Next/Skip go on to the one-page "Online features
// and printer connections" question (the Bambu Lab network plug-in), which finishes the guide.
function NextPage()
{
	window.location.href="../4unlayered/index.html";
}
