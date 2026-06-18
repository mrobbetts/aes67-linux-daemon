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

// Add a card. A card is card-level only ({name, domain}); PCMs are added to it
// afterwards from the Cards tree. Name is the durable identity.
class CardEdit extends Component {
  static propTypes = {
    applyEdit: PropTypes.func.isRequired,
    closeEdit: PropTypes.func.isRequired,
    editIsOpen: PropTypes.bool.isRequired
  };

  constructor(props) {
    super(props);
    this.state = { name: '', nameErr: true, domain: 0 };
    this.onSubmit = this.onSubmit.bind(this);
    this.onCancel = this.onCancel.bind(this);
  }

  componentDidMount() {
    Modal.setAppElement('body');
  }

  onSubmit() {
    RestAPI.addCard(this.state.name, this.state.domain).then(function() {
      toast.success('Card ' + this.state.name + ' added');
      this.props.applyEdit();
    }.bind(this));
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
          <h2><center>Add Card</center></h2>
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
