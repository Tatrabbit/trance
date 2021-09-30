#include "subroutine_list.h"
#include "common/common.h"

namespace
{
  const std::string SUBROUTINE_ITEM_TOOLTIP =
      "A subplaylist invoked by this subroutine. When the subplaylist finishes "
      "because there is no next playlist item available, control returns to the "
      "subroutine. When there are no more subplaylists, a new playlist item is "
      "chosen from the next playlist items of the subroutine.";
}

SubroutineList::SubroutineList(wxWindow* parent, wxWindowID id) : wxScrolledWindow(parent, id)
{
  _choice_items = new std::vector<wxChoice*>;

  SetMinSize(wxSize(-1, 200));

	_sizer = new wxBoxSizer(wxVERTICAL);
	SetSizer(_sizer);

	SetScrollRate(10, 5);
}

SubroutineList::~SubroutineList()
{
  delete _choice_items;
}

wxChoice *SubroutineList::AddItem(
	const std::vector<std::string>& items,
	const std::string& selected_item,
	size_t &index)
{
	auto choice = new wxChoice{this, wxID_ANY};
	choice->SetToolTip(SUBROUTINE_ITEM_TOOLTIP);
	_sizer->Add(choice, 0, wxALL|wxEXPAND, DEFAULT_BORDER);

	choice->Append("");
	int i = 1;
	for (const auto& item_name : items) {
	  choice->Append(item_name);
	  if (item_name == selected_item) {
		choice->SetSelection(i);
	  }
	  ++i;
	}
	if (selected_item.empty()) {
	  choice->SetSelection(0);
	}

	index = _choice_items->size();
	_choice_items->push_back(choice);

  FitInside();
	Layout();
  return choice;
}

void SubroutineList::Clear()
{
	_sizer->Clear(true);
  _choice_items->clear();
}
