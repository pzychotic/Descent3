/*
 * Descent 3
 * Copyright (C) 2024 Parallax Software
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// GenericDeathDialog.cpp : implementation file
//

#include "stdafx.h"
#include "editor.h"
#include "GenericDeathDialog.h"
#include "pserror.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#undef THIS_FILE
static char THIS_FILE[] = __FILE__;
#endif

/////////////////////////////////////////////////////////////////////////////
// CGenericDeathDialog dialog

CGenericDeathDialog::CGenericDeathDialog(object_info *objinfo, CWnd *pParent /*=NULL*/)
    : CDialog(CGenericDeathDialog::IDD, pParent) {
  //{{AFX_DATA_INIT(CGenericDeathDialog)
  m_prob1 = 0;
  m_prob2 = 0;
  m_prob3 = 0;
  m_prob4 = 0;
  //}}AFX_DATA_INIT

  // Store ptr to source data
  m_objinfo = objinfo;

  ASSERT(MAX_DEATH_TYPES == 4);

  // Copy data
  for (int i = 0; i < MAX_DEATH_TYPES; i++)
    m_death_types[i] = objinfo->death_types[i];

  m_prob1 = objinfo->death_probabilities[0];
  m_prob2 = objinfo->death_probabilities[1];
  m_prob3 = objinfo->death_probabilities[2];
  m_prob4 = objinfo->death_probabilities[3];
}

void CGenericDeathDialog::DoDataExchange(CDataExchange *pDX) {
  CDialog::DoDataExchange(pDX);
  //{{AFX_DATA_MAP(CGenericDeathDialog)
  DDX_Text(pDX, IDC_GENEREIC_DEATH_PROB1, m_prob1);
  DDX_Text(pDX, IDC_GENEREIC_DEATH_PROB2, m_prob2);
  DDX_Text(pDX, IDC_GENEREIC_DEATH_PROB3, m_prob3);
  DDX_Text(pDX, IDC_GENEREIC_DEATH_PROB4, m_prob4);
  //}}AFX_DATA_MAP
}

BEGIN_MESSAGE_MAP(CGenericDeathDialog, CDialog)
//{{AFX_MSG_MAP(CGenericDeathDialog)
ON_BN_CLICKED(IDC_GENERIC_DEATH_EDIT1, OnGenericDeathEdit1)
ON_BN_CLICKED(IDC_GENERIC_DEATH_EDIT2, OnGenericDeathEdit2)
ON_BN_CLICKED(IDC_GENERIC_DEATH_EDIT3, OnGenericDeathEdit3)
ON_BN_CLICKED(IDC_GENERIC_DEATH_EDIT4, OnGenericDeathEdit4)
//}}AFX_MSG_MAP
END_MESSAGE_MAP()

/////////////////////////////////////////////////////////////////////////////
// CGenericDeathDialog message handlers

#include "DeathDialog.h"

void CGenericDeathDialog::OnGenericDeathEdit1() {
  CDeathDialog dlg(&m_death_types[0]);

  dlg.DoModal();
}

void CGenericDeathDialog::OnGenericDeathEdit2() {
  CDeathDialog dlg(&m_death_types[1]);

  dlg.DoModal();
}

void CGenericDeathDialog::OnGenericDeathEdit3() {
  CDeathDialog dlg(&m_death_types[2]);

  dlg.DoModal();
}

void CGenericDeathDialog::OnGenericDeathEdit4() {
  CDeathDialog dlg(&m_death_types[3]);

  dlg.DoModal();
}

void CGenericDeathDialog::OnOK() {
  // Get the data
  UpdateData(true);

  int total_prob = m_prob1 + m_prob2 + m_prob3 + m_prob4;

  if ((total_prob != 100) && (total_prob != 0)) {
    OutrageMessageBox("The total of all death probabilities must be 100 or 0.");
    return;
  }

  for (int i = 0; i < MAX_DEATH_TYPES; i++)
    m_objinfo->death_types[i] = m_death_types[i];

  m_objinfo->death_probabilities[0] = m_prob1;
  m_objinfo->death_probabilities[1] = m_prob2;
  m_objinfo->death_probabilities[2] = m_prob3;
  m_objinfo->death_probabilities[3] = m_prob4;

  CDialog::OnOK();
}
