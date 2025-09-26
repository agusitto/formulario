const mongoose = require('../db');

const UsuarioSchema = new mongoose.Schema({
  nombre: String,
  email: String
});

module.exports = mongoose.model('Usuario', UsuarioSchema);
