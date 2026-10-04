// UNIQUE FUNCTIONS

// Keep in here for future additions
function OnInit()
{
	//let strInput=JSON.stringify(cData);
	//HandleModelList(cData);
	
	TranslatePage();
	RequestProfile();
}
 
function GotoFilamentPage()
{
	let nChoose=OnExitFilter();
	
	if(nChoose>0)
		window.open('../0/index.html?target=22&wait=detail','_self');
}
