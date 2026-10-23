// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
}
 
// Unlayered 0.1.7: Back goes to the "Settings from other slicers" step (guide 5import) when the
// wizard showed it, else to the privacy page; the slicer knows which (HandleStudio, page_before_models).
function GoBackFromModels()
{
	var tSend={};
	tSend['sequence_id']=Math.round(new Date() / 1000);
	tSend['command']="request_page_before_models";

	SendWXMessage( JSON.stringify(tSend) );
}

function GotoFilamentPage()
{
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
		window.open('../0/index.html?target=22&wait=detail','_self');
}
