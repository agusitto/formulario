const express = require('express');
const cors = require('cors');
const bodyParser = require('body-parser');
const Usuario = require('./models/Usuario');

const app = express();
app.use(cors());
app.use(bodyParser.json());

app.post('/api/form', async (req, res) => {
  try {
    const usuario = new Usuario(req.body);
    await usuario.save();
    res.json({ status: 'OK', data: usuario });
  } catch (err) {
    res.status(500).json({ error: 'error al guardar' });
  }
});

app.listen(3000, () => console.log('srv en: http://192.168.1.7:3000'));
