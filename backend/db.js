const mongoose = require('mongoose');

mongoose.connect('mongodb://localhost:27017/formulario')
.then(() => console.log('se ejecuto el mongo'))
.catch(err => console.error(err));

module.exports = mongoose;
