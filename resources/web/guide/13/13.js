// Unlayered 0.1.6: the privacy page (see index.html). The slicer decides what is preselected
// (privacy_state), records whether a preselected answer was merely kept (never reported as an
// explicit choice), saves the answer, and only then does the flow continue. The improvement-program
// box is sent with the answer; it is always an explicit tick, since it never starts ticked.

var m_State=null;
var m_Preselected='';   // the answer the page started with ('' = none)
var m_Touched=false;    // the person changed the selection themselves
var m_Saving=false;

function OnInit()
{
	TranslatePage();

	$('input[name=diagnostics]').on('change', function() { m_Touched=true; UpdateState(); });
	$('#Program').on('change', function() { UpdateState(); });

	SendPrivacy({'command':'privacy_request_state'});
}

function SendPrivacy( tSend )
{
	tSend['sequence_id']=Math.round(new Date() / 1000);
	SendWXMessage( JSON.stringify(tSend) );
}

function HandleStudio( pVal )
{
	let strCmd=pVal['command'];

	if( strCmd=='privacy_state' )
	{
		m_State=pVal;
		$('#OrgNotice').prop('hidden', !pVal['organization_off']);
			// "Recommended" only where Share may start selected (the United States).
		$('#Recommended').prop('hidden', !pVal['preselect_share']);
		if( pVal['preselect_share'] && !m_Touched )
		{
			m_Preselected='share';
			$('input[name=diagnostics][value=share]').prop('checked', true);
		}
		UpdateState();
		// Counted once per flow by the slicer, however often this page is shown.
		SendPrivacy({'command':'privacy_page_shown'});
	}
	else if( strCmd=='privacy_saved' )
	{
		m_Saving=false;
		if( !pVal['ok'] )
		{
			$('#SaveError').prop('hidden', false);
			UpdateState();
			return;
		}
		if( m_State && m_State['privacy_only'] )
			SendPrivacy({'command':'privacy_finish'});
		else
			window.location.href="../21/index.html";
	}
}

function Choice()
{
	let v=$('input[name=diagnostics]:checked').val();
	return v ? v : '';
}

function UpdateState()
{
	// The program box only exists under Share; leaving Share unticks it (it never starts ticked).
	let share=(Choice()=='share');
	if( !share )
		$('#Program').prop('checked', false);
	$('#ProgramBox').prop('hidden', !share);
	$('#ContinueBtn').prop('disabled', m_State==null || m_Saving || Choice()=='');
}

function GoContinue()
{
	let choice=Choice();
	if( choice=='' || m_State==null )
		return;
	m_Saving=true;
	UpdateState();
	SendPrivacy({'command':'privacy_submit',
	             'choice':choice,
	             'program': (choice=='share' && $('#Program').prop('checked')),
	             'kept_preselection': (m_Preselected!='' && !m_Touched && choice==m_Preselected)});
}

function GoBack()
{
	window.location.href="../11/index.html";
}

function OpenPolicy()
{
	SendPrivacy({'command':'privacy_open_policy'});
}
