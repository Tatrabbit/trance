#ifndef TRANCE_SRC_CREATOR_SUBROUTINE_LIST_H
#define TRANCE_SRC_CREATOR_SUBROUTINE_LIST_H
#include <string>

#pragma warning(push, 0)
#include <wx/wx.h>
#pragma warning(pop)

#pragma once

class SubroutineList : public wxScrolledWindow
{
public:
  SubroutineList(wxWindow* parent, wxWindowID id);
  ~SubroutineList();
  wxChoice* AddItem(const std::vector<std::string> &items, const std::string& selected_item, size_t& index);
  void Clear();
  //inline wxSizer* GetSizer() const { return _sizer; }

private:
  wxBoxSizer* _sizer;
  std::vector<wxChoice*>* _choice_items;
};

#endif
