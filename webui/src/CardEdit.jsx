//
//  CardEdit.jsx
//
//  Copyright (c) 2019 2020 Andrea Bondavalli. All rights reserved.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//

import React, {Component} from 'react';
import PropTypes from 'prop-types';
import {toast} from 'react-toastify';
import Modal from 'react-modal';

import RestAPI from './Services';

const editCustomStyles = {
  content : {
    top: '50%', left: '50%', right: 'auto', bottom: 'auto',
    marginRight: '-50%', transform: 'translate(-50%, -50%)'
  }
};

// Add or edit a card. A card is card-level only ({name, domain}); PCMs are added
// from the Cards tree. Edit renames and/or re-domains it (recreate-card) -- which
// changes the hw:<name> ALSA id and, for a non-zero domain, enters W11.
class CardEdit extends Component {
  static propTypes = {
    applyEdit: PropTypes.func.isRequired,
    closeEdit: PropTypes.func.isRequired,
    editIsOpen: PropTypes.bool.isRequired,
    isEdit: PropTypes.bool,        // edit (rename/re-domain) vs add
    card: PropTypes.object         // present on edit
  };

  constructor(props) {
    super(props);
    const card = this.props.card || {};
    this.state = {
      name: card.name || '',
      nameErr: !(card.name && card.name.length > 0),
      domain: card.domain !== undefined ? card.domain : 0
    };
    this.onSubmit = this.onSubmit.bind(this);
    this.onCancel = this.onCancel.bind(this);
  }

  componentDidMount() {
    Modal.setAppElement('body');
  }

  onSubmit() {
    const done = function() {
      toast.success('Card ' + this.state.name + (this.props.isEdit ? ' updated' : ' added'));
      this.props.applyEdit();
    }.bind(this);
    if (this.props.isEdit) {
      RestAPI.updateCard(this.props.card.name, this.state.name, this.state.domain).then(done);
    } else {
      RestAPI.addCard(this.state.name, this.state.domain).then(done);
    }
  }

  onCancel() {
    this.props.closeEdit();
  }

  render() {
    return (
      <div id='card-edit'>
        <Modal ariaHideApp={false}
          isOpen={this.props.editIsOpen}
          onRequestClose={this.props.closeEdit}
          style={editCustomStyles}
          contentLabel='Card Edit'>
          <h2><center>{this.props.isEdit ? 'Edit Card "' + this.props.card.name + '"' : 'Add Card'}</center></h2>
          { this.props.isEdit ?
            <p style={{textAlign: 'center', color: '#a60', maxWidth: '32em'}}>
              Renaming changes the card's <code>hw:&lt;name&gt;</code> ALSA id — update any
              CamillaDSP/clients pointing at it. A non-zero PTP domain is multi-domain
              (W11, untested) — the card only locks if a PTP master exists on that domain.
              Either change briefly interrupts this card's audio.
            </p> : undefined }
          <table><tbody>
            <tr>
              <th align='left'> <label>Name</label> </th>
              <th align='left'> <input value={this.state.name}
                onChange={e => this.setState({name: e.target.value, nameErr: e.target.value.length === 0})}
                required/> </th>
            </tr>
            <tr>
              <th align='left'> <label>PTP domain</label> </th>
              <th align='left'> <input type='number' min='0' max='127' className='input-number'
                value={this.state.domain}
                onChange={e => this.setState({domain: e.target.value})}/> </th>
            </tr>
          </tbody></table>
          <br/>
          <div style={{textAlign: 'center'}}>
            <button onClick={this.onSubmit} disabled={this.state.nameErr ? true : undefined}>Submit</button>
            &nbsp;&nbsp;&nbsp;&nbsp;
            <button onClick={this.onCancel}>Cancel</button>
          </div>
        </Modal>
      </div>
    );
  }
}

export default CardEdit;
